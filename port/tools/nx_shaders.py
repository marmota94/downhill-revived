"""The Switch's shaders: native.wgsl (and zread.rs's probe) through naga to GLSL, explicit bindings,
then uam to deko3d's DKSH, embedded as C arrays for port/nx/src/dhd_render_nx.c.

    python port/tools/nx_shaders.py OUT.inc   (NAGA names naga-cli 29.0.3, the repo's version)

naga's GLSL has no binding qualifiers and refuses textureLoad on depth textures, so: a depth texture
is read as texture_2d<f32>'s red channel (deko3d's ZF32 view does that), and each resource gets its
WGSL binding number as its slot (uniform blocks, storage buffers and images, one each here, slot 0).
--keep-coordinate-space: deko3d's clip space is already WebGPU's (y up, z 0..1).

What stock uam (1.1.0) gets wrong is worked around here, each with a check that stops the build:
- integer division or modulo by a non-constant becomes a float multiply by a reciprocal that can land
  just under the integer (uam warns; the warning is fatal here). The WGSL below uses shifts for the
  powers of two and nx_div's half-offset form, exact for these small values, for the rest.
- a dynamic index into a vector's components reads .x (nx_pick).
- its dual-issue scheduling corrupts warp-sized blocks of pixels (no_dual_issue).
- it gives a graphics stage no off-chip call/return stack, whatever its branches' nesting (with_crs).
- its schedule leaves a hazard in the draw stages that draws warp-sized blocks of wrong texels
  (draw_schedule; finding 270).
native_convert runs as a pixel shader into a render target, the path everything else already proves:
the one compute shader that wrote an image (native_scanout, now a blit) came out in broken blocks."""
import os, re, struct, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KIT = Path(os.environ.get('DEVKITPRO') or '/opt/devkitpro')
NAGA = os.environ.get('NAGA') or str(Path.home() / 'DevCaches/naga/naga')
ENTRIES = (('native_vertex', 'vert'), ('native_mesh_vertex', 'vert'), ('native_pixel', 'frag'), ('native_pixel_dual', 'frag'),
           ('fullscreen_vertex', 'vert'), ('native_blit', 'frag'), ('native_upload_alpha', 'frag'),
           ('native_grow_depth', 'frag'), ('native_convert', 'frag'), ('present_pixel', 'frag'), ('present_direct', 'frag'))
DRAW_ENTRIES = ('native_vertex', 'native_pixel', 'native_pixel_dual')
DIVISION_WARNING = 'non-constant integer division'


def wgsl():
    gs = ROOT / 'port/gs'
    text = 'enable dual_source_blending;\n' + (gs / 'native.wgsl').read_text() + (gs / 'native_dual.wgsl').read_text()
    text = text.replace('texture_depth_2d', 'texture_2d<f32>')
    # a depth texture is read as a colour one (deko3d binds a depth image as a texture): its loads give
    # the .r of the vec4 where the shader wants the depth. Main's native.wgsl loads them in the grow
    # entry point and in load_convert_depth0..3 (the old load_depth helper is gone).
    text, n = re.subn(r'(return textureLoad\((?:grow_source|convert_depth[0-3]), xy, 0\))(;)', r'\1.r\2', text)
    if n != 5:
        sys.exit('nx_shaders: expected 5 depth loads, found %d' % n)
    # the conversion's: its result as a colour, its table a texture
    text += '''
fn nx_unpack(w: u32) -> vec4<f32> {
    return vec4<f32>(f32(w & 255u), f32(w >> 8u & 255u), f32(w >> 16u & 255u), f32(w >> 24u)) / 255.0;
}
// The inverse table as a 128 x 64 RGBA8 texture (dhd_render_nx.c): x in red, y in green.
fn nx_inverse(i: u32) -> u32 {
    let c = vec2<u32>(round(textureLoad(native_inverse, vec2<i32>(i32(i & 127u), i32(i >> 7u)), 0).rg * 255.0));
    return c.x | c.y << 8u;
}
'''
    return patched(text, CONVERT_PATCHES + TEXEL_PATCHES)


def patched_text(text, patches):
    """text with each patch applied (all must apply)."""
    for old, new in patches:
        if old not in text:
            sys.exit('nx_shaders: %s not found' % old.strip().split('\n')[0])
        text = text.replace(old, new)
    return text


def patched(text, patches):
    """text with the uam workarounds' helpers appended and each patch applied (all must apply)."""
    # uam loses a dynamic index into a vector's components (it reads .x): by select instead
    text += '''
fn nx_pick(v: vec4<u32>, i: u32) -> u32 {
    if (i == 0u) { return v.x; }
    if (i == 1u) { return v.y; }
    if (i == 2u) { return v.z; }
    return v.w;
}
// a / b for small values: halfway between two multiples of b, a rounding error can't cross one.
fn nx_div(a: u32, b: u32) -> u32 {
    if (b == 0u) { return a; }
    return u32(floor((f32(a) + 0.5) / f32(b)));
}
fn nx_mod(a: u32, b: u32) -> u32 {
    if (b == 0u) { return 0u; }
    return a - nx_div(a, b) * b;
}
'''

    for old, new in patches:
        if old not in text:
            sys.exit('nx_shaders: %s not found' % old)
        text = text.replace(old, new)
    return text


# the texel lookup's scales: small, not always powers of two
TEXEL_PATCHES = (('let step = S / E;', 'let step = nx_div(S, E);'),
                 ('floor(vec2<f32>(c) / f32(E))', 'floor((vec2<f32>(c) + 0.5) / f32(E))'))
CONVERT_PATCHES = (('convert.pages[page >> 4u][(page >> 2u) & 3u]', 'nx_pick(convert.pages[page >> 4u], (page >> 2u) & 3u)'),
                   ('convert.palette[value >> 2u][value & 3u]', 'nx_pick(convert.palette[value >> 2u], value & 3u)'),
                   # pages: 64 or 128 wide, 32, 64 or 128 tall
                   ('let px = x % pw;', 'let px = x & (pw - 1u);'),
                   ('let py = y % ph;', 'let py = y & (ph - 1u);'),
                   ('((y / ph) * (bw * 64u / pw) + x / pw)',
                      '((y >> firstTrailingBit(ph)) * ((bw * 64u) >> firstTrailingBit(pw)) + (x >> firstTrailingBit(pw)))'),
                   # a surface's width in pages: anything
                   ('let origin = vec2<u32>((rel % bw) * 64u, (rel / bw) * 64u);',
                      'let origin = vec2<u32>(nx_mod(rel, bw) * 64u, nx_div(rel, bw) * 64u);'),
                   ('i32(((rel % bw) * 64u + (at & 255u)) * scale + scale / 2u)',
                      'i32((nx_mod(rel, bw) * 64u + (at & 255u)) * scale + scale / 2u)'),
                   ('i32(((rel / bw) * 32u + (at >> 8u)) * scale + scale / 2u)',
                      'i32((nx_div(rel, bw) * 32u + (at >> 8u)) * scale + scale / 2u)'),
                   # the conversion as a pixel shader, its table a texture
                   ('var<storage, read> native_inverse: array<u32>', 'var native_inverse: texture_2d<f32>'),
                   ('native_inverse[half]', 'nx_inverse(half)'),
                   ('native_inverse[half + 1u]', 'nx_inverse(half + 1u)'),
                   ('native_inverse[select(0u, 2048u, s.z == 1u) + (word & 2047u)]', 'nx_inverse(select(0u, 2048u, s.z == 1u) + (word & 2047u))'),
                   ("""@compute @workgroup_size(8, 8, 1)
fn native_convert(@builtin(global_invocation_id) tid: vec3<u32>) {
    if (tid.x >= convert.size.x || tid.y >= convert.size.y) { return; }
""", """@fragment
fn native_convert(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let tid = vec2<u32>(position.xy);
"""),
                   # main's base texture is u32 words and the stores are r32uint; deko3d's textures are
                   # RGBA8, so the base is read as a colour, packed, and the result written as a colour
                   ('var convert_base: texture_2d<u32>', 'var convert_base: texture_2d<f32>'),
                   ('return textureLoad(convert_base, xy, 0).x;',
                      'let b = vec4<u32>(round(textureLoad(convert_base, xy, 0) * 255.0));\n    return b.r | b.g << 8u | b.b << 16u | b.a << 24u;'),
                   ("""textureStore(native_converted, tid.xy, vec4<u32>(load_convert_base(vec2<i32>(tid.xy)), 0u, 0u, 0u));
        return;""", 'return nx_unpack(load_convert_base(vec2<i32>(tid.xy)));'),
                   ('textureStore(native_converted, tid.xy, vec4<u32>(rgba, 0u, 0u, 0u));', 'return nx_unpack(rgba);'))


def probe():
    source = (ROOT / 'port/render/src/native/zread.rs').read_text()
    m = re.search(r'const ZREAD_WGSL: &str = r#"(.*?)"#;', source, re.S)
    text = m.group(1).replace('texture_depth_2d', 'texture_2d<f32>')
    return text.replace('bitcast<u32>(textureLoad(zread_source, vec2<i32>(p), 0))', 'bitcast<u32>(textureLoad(zread_source, vec2<i32>(p), 0).r)')


def bind(glsl):
    glsl = re.sub(r'^uniform (\w*sampler\w+) (_group_\d+_binding_(\d+)_\w+);',
                  lambda m: 'layout(binding = %s) uniform %s %s;' % (m[3], m[1], m[2]), glsl, flags=re.M)
    glsl = re.sub(r'^layout\(std140\) uniform', 'layout(std140, binding = 0) uniform', glsl, flags=re.M)
    glsl = re.sub(r'^layout\(std430\) ', 'layout(std430, binding = 0) ', glsl, flags=re.M)
    glsl = re.sub(r'^layout\(rgba8\) ', 'layout(rgba8, binding = 0) ', glsl, flags=re.M)
    return glsl


def no_dual_issue(dksh, raw, entry):
    """Stock uam schedules pairs of instructions to issue together, and warp-sized blocks of pixels come
    out wrong (halo3-nx HowToDeko3d, section 3.2, patch 7: found by bisecting on the console). uam has no
    switch for it, but the schedule is plain data: every fourth 64-bit word of the code holds 21 bits for
    each of the next three instructions. setDelay in nv50_ir_emit_gm107.cpp marks a pair with stall 0 and
    the yield bit, which nothing else sets, and only where the stall would otherwise be 1. So stall 1
    without yield is what it writes with dual issue off; what follows then runs a cycle later than the
    schedule counted, which only gives results more time. NX_DUAL_ISSUE=1 keeps uam's."""
    magic, _, control, _, _, _ = struct.unpack_from('<6I', dksh)
    at = dksh.find(raw)
    # the code follows the control section, after the pixel or vertex shader's 128-byte header
    if magic != 0x48534B44 or at not in (control, control + 128) or len(raw) % 32:
        sys.exit('nx_shaders: %s: unexpected DKSH layout (code at %d of %d)' % (entry, at, len(dksh)))
    out = bytearray(dksh)
    pairs = 0
    for group in range(at, at + len(raw), 32):
        word = int.from_bytes(out[group:group + 8], 'little')
        if word >> 63:
            sys.exit('nx_shaders: %s: not a schedule word at %d' % (entry, group))
        for k in range(3):
            if word >> (21 * k) & 0x1F == 0x10:
                word ^= 0x11 << (21 * k)
                pairs += 1
        out[group:group + 8] = word.to_bytes(8, 'little')
    return bytes(out), pairs


def draw_schedule(dksh, raw, at):
    """The draw stages' schedule without hazards: every instruction waits for all six dependency
    barriers, stalls the longest (15 cycles) and sets no operand reuse. uam's own schedule for main's
    draw stages drew warp-sized blocks of wrong texels on the console although the code computes the
    right values in order (finding 270); with this one they draw right, and faster than 0.7.6's
    stages did with uam's schedule. at: where the code starts in dksh."""
    out = bytearray(dksh)
    for group in range(at, at + len(raw), 32):
        word = int.from_bytes(out[group:group + 8], 'little')
        for k in range(3):
            c = word >> (21 * k) & 0x1FFFFF
            c = (c & 0x7E0) | 0x1F800 | 15          # its own barriers kept; waits on all, stall 15, no yield or reuse
            word = word & ~(0x1FFFFF << (21 * k)) | c << (21 * k)
        out[group:group + 8] = word.to_bytes(8, 'little')
    return bytes(out)


def branch_depth(raw):
    """The deepest nesting of SSY, PBK and PCNT (each pushes a reconvergence entry on the warp's
    call/return stack, popped by SYNC, BRK or CONT) over every path through the Maxwell code raw:
    32-byte groups of a schedule word and three instructions; a branch target is a signed 24-bit
    offset (bits 20-43) from the next instruction (uam aims at groups); the guard predicate is
    bits 16-19 (7: always)."""
    code = {}
    for group in range(0, len(raw), 32):
        for k in range(1, 4):
            code[group + 8 * k] = int.from_bytes(raw[group + 8 * k:group + 8 * k + 8], 'little')
    order = sorted(code)
    following = dict(zip(order, order[1:] + [None]))

    def target(at, word):
        offset = word >> 20 & 0xFFFFFF
        to = following[at] + (offset - (1 << 24) if offset >> 23 else offset)
        return to + 8 if to % 32 == 0 else to       # a group's start: its schedule word, then code

    push, pop = (0xE29, 0xE2A, 0xE2B), (0xF0F, 0xE34, 0xE35)    # ssy pbk pcnt / sync brk cont
    seen, work, deepest = {}, [(order[0], ())], 0
    while work:
        at, stack = work.pop()
        if at is None or at not in code or seen.get(at, -1) >= len(stack):
            continue
        seen[at] = len(stack)
        deepest = max(deepest, len(stack))
        word = code[at]
        op, always = word >> 52, (word >> 16 & 0xF) == 7
        if op in push:
            work.append((following[at], stack + (target(at, word),)))
        elif op in pop and stack:
            work.append((stack[-1], stack[:-1]))
            if not always:
                work.append((following[at], stack))
        elif op == 0xE24:                                           # bra
            work.append((target(at, word), stack))
            if not always:
                work.append((following[at], stack))
        elif op != 0xE30 or not always:                             # exit
            work.append((following[at], stack))
    return deepest


# The queue's scratch memory per warp (dkQueueMakerDefaults in dhd_render_nx.c), which holds a
# stage's local memory and its off-chip call/return stack.
PER_WARP_SCRATCH = 0x800


def with_crs(dksh, raw, entry):
    """Stock uam writes a call/return stack size of 0 into every graphics stage's header ("TODO:
    Proper logic"; compute gets 0x800). The on-chip stack holds 16 entries, two a nesting level (a
    reconvergence point and a divergent branch's other side), and what doesn't fit goes to that
    off-chip stack: with none, a warp nested deeper than 8 loses its reconvergence state and its
    pixels come out wrong in blocks (finding 270). Mesa's nvc0_program.c has the size (commented
    out): ((depth * 2 + 47) / 48) * 0x200 bytes a warp when depth * 2 > 16. Here a deep stage gets
    all of the warp's scratch that its local memory leaves (0x200 multiples, the header's unit)."""
    magic, _, control, _, programs, _ = struct.unpack_from('<6I', dksh)
    at = dksh.find(raw)
    if magic != 0x48534B44 or at != control + 128:
        return dksh, 0
    depth = branch_depth(raw)
    if depth * 2 <= 16:
        return dksh, depth
    sph = at - 0x50                          # the 0x50-byte shader program header ends at the code
    local = struct.unpack_from('<I', dksh, sph + 4)[0] & 0xFFFFFF
    crs = (PER_WARP_SCRATCH - local * 32) & ~0x1FF
    if crs < (depth * 2 + 47) // 48 * 0x200:
        sys.exit('nx_shaders: %s nests %d deep: no room for its call/return stack' % (entry, depth))
    out = bytearray(dksh)
    word3 = struct.unpack_from('<I', out, sph + 12)[0]
    struct.pack_into('<I', out, sph + 12, word3 & ~0xFFFFFF | crs)
    struct.pack_into('<I', out, programs + 20, local * 32 + crs)    # DkshProgramHeader.per_warp_scratch_sz
    return bytes(out), depth


def build(work, source, entry, stage):
    glsl = work / ('%s.%s' % (entry, stage))
    r = subprocess.run([NAGA, str(source), str(glsl), '--entry-point', entry, '--profile', 'core460', '--keep-coordinate-space'],
                       capture_output=True, text=True, env=dict(os.environ, NO_COLOR='1'))
    if r.returncode:
        sys.exit('naga %s:\n%s%s' % (entry, r.stdout, r.stderr))
    glsl.write_text(bind(glsl.read_text()))
    dksh = work / (entry + '.dksh')
    raw = work / (entry + '.raw')
    r = subprocess.run([str(KIT / 'tools/bin/uam'), '-s', stage, '-o', str(dksh), '-r', str(raw), str(glsl)], capture_output=True, text=True)
    if r.returncode:
        sys.exit('uam %s:\n%s%s\n%s' % (entry, r.stdout, r.stderr, glsl.read_text()[:4000]))
    messages = (r.stdout + r.stderr).strip()
    if messages:
        print('uam %s: %s' % (entry, messages))
    if DIVISION_WARNING in messages:
        sys.exit('nx_shaders: %s divides by a non-constant, which uam turns into an inexact float: rewrite it in wgsl()' % entry)
    data, code = dksh.read_bytes(), raw.read_bytes()
    code_at = data.find(code)
    data, depth = with_crs(data, code, entry)
    pairs = 0
    if not os.environ.get('NX_DUAL_ISSUE'):
        data, pairs = no_dual_issue(data, code, entry)
    if entry in DRAW_ENTRIES:
        data = draw_schedule(data, code, code_at)
    return data, pairs, depth


def main():
    out = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as t:
        work = Path(t)
        (work / 'native.wgsl').write_text(wgsl())
        (work / 'zread.wgsl').write_text(probe())
        lines = ["/* port/tools/nx_shaders.py: DKSH of port/gs/native.wgsl and zread.rs's probe. */"]
        for name, source, stage in [(e, work / 'native.wgsl', s) for e, s in ENTRIES] + [('zread', work / 'zread.wgsl', 'comp')]:
            data, pairs, depth = build(work, source, name, stage)
            lines.append('static const unsigned char dksh_%s[] __attribute__((aligned(256))) = {%s};' % (name, ','.join(map(str, data))))
            print('%s.%s: %d bytes, %d dual-issue pairs undone, branches nested %d deep' % (name, stage, len(data), pairs, depth))
        out.write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
