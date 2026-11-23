/* The GS renderer on the Switch: dhd_render.h's native half on deko3d, in place of the Rust library
   (port/render/src/native/ on wgpu), which can't run here. The same picture, command for command:
   render.rs's loop, surfaces.rs's copies and blits, convert.rs, scanout.rs, present.rs, zread.rs.
   Dual-source blending is always there, so render.rs's two-pass fallback is not.

   Shaders: port/tools/nx_shaders.py (native.wgsl through naga and uam). deko3d has no pipelines, so
   a draw's key sets state directly; nothing is compiled while playing.

   Threads: textures may be made and released on any thread (the allocator's lock; a new texture's
   pixels wait for the render thread); everything else runs on the render thread. The exact renderer
   (compute rasterizer) stays off: native mode reads its own depth.

   Unlike the Rust, conversions are a pixel shader into a render target and scanout a 2D blit: uam's
   one compute shader that wrote an image came out in broken blocks. Only the Z probe is compute.

   Barriers (deko3d's own source): Fragments waits for idle and is what makes rendered pixels readable;
   Full also stops command prefetch, so only compute, which Fragments doesn't wait for, gets it. Every
   dkQueueFlush invalidates the descriptor caches at the start of the next submission, and descriptors
   are written before the commands using them are submitted, so none is invalidated by hand.
   dkCmdBufCopyImage and dkCmdBufCopyBufferToImage use the copy engine: Fragments and an image
   invalidate follow each. */
#include "dhd_render.h"
#include "dhd_render_ext.h"
#include <deko3d.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "nx_shaders.inc"

enum {
    FRAMES = 3,
    ARENA_SIZE = 128 << 20,          /* a frame's vertices, indices, constants and uploads */
    CMD_SIZE = 16 << 20, CMD_CHUNK = 1 << 20,
    HEAP_SIZE = 1024 << 20,          /* surfaces and textures */
    HEAP_LOW = 192 << 20,            /* below this much free, the texture cache gives back */
    CODE_SIZE = 1 << 20,
    IMAGE_DESCRIPTORS = 8192,  /* the game keeps up to NATIVE_TEXTURES = 4096 textures, plus surfaces and views */ SAMPLER_DESCRIPTORS = 16,
    SURFACES = 48, CONVERTS = 64,
    IMPORT_SIDE = 64,
    ZREAD_MAX = 64, ZREAD_SLOTS = 3, ZREAD_BYTES = ZREAD_MAX * ZREAD_MAX * 4,
    INVERSE_WORDS = 8192, INVERSE_W = 128, INVERSE_H = 64,
    SWAP_IMAGES = 3,
};
enum { SAMPLER_NEAREST, SAMPLER_LINEAR, SAMPLER_REPLACE };  /* then 8 replacement samplers */
enum { BLEND_SA = 5, BLEND_ISA = 6, BLEND_FACTOR = 14, BLEND_INV_FACTOR = 15 };

/* What the render thread is doing, for the main thread's heartbeat (nx_host.c). */
volatile const char *nx_render_stage = "not started";
volatile uint64_t nx_pictures, nx_presents, nx_draws, nx_shown, nx_commands;
/* Nanoseconds on the render thread, none counted twice: recording pictures and probes; every submit;
   waiting for the frame's fence; waiting for a screen image; recording and queueing the present. */
volatile uint64_t nx_t_record_ns, nx_t_submit_ns, nx_t_gpuwait_ns, nx_t_acquire_ns, nx_t_present_ns;
/* Barriers that wait (Fragments or Full: the GPU drains its work before what follows), for the heartbeat's
   barriers a field: what the renderer gains of finding 270 step 3 cut on this GPU, where a pass costs
   no load or store but each such wait idles the 3D engine. */
volatile uint64_t nx_barriers;
static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

void nx_screen_size(int *w, int *h);   /* sdl_nx.c: decided once at startup */

static void nx_log(const char *text) { dhd_host_log(text, strlen(text)); }
static void nx_logf(const char *format, ...)
{
    char line[256];
    va_list a;
    va_start(a, format);
    int n = vsnprintf(line, sizeof line, format, a);
    va_end(a);
    if (n > 0) dhd_host_log(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

/* ---- the device ---- */
static DkDevice device;
static DkQueue queue;
static DkCmdBuf cmd;
static void barrier(DkBarrier mode, uint32_t invalidate)
{
    if (mode != DkBarrier_None) nx_barriers++;
    dkCmdBufBarrier(cmd, mode, invalidate);
}
static DkMemBlock code_block, heap_block, descriptor_block, zread_block, capture_block;
static DhdrSurface *screen;  /* the live surface, for nx_render_release */
static DkImageDescriptor *image_descriptors;
static DkSamplerDescriptor *sampler_descriptors;
static DkGpuAddr image_set, sampler_set;
static Mutex lock;   /* the heap, the descriptors, the upload and free lists */
static int ready;
static const char *adapter = "";
static int strict_barriers;   /* DHD_NX_BARRIERS: a barrier after every pass, to tell a missing one on the console */
int nx_gpu_error(void) { return queue && dkQueueIsInErrorState(queue); }

/* A GPU fault: the next queue call would end the process without a word. Before each of them. */
static void queue_check(const char *where)
{
    if (!dkQueueIsInErrorState(queue)) return;
    nx_logf("gs native: GPU fault (queue in error state) before %s; stage %s, picture %llu, %llu commands", where,
            (const char *)nx_render_stage, (unsigned long long)nx_pictures, (unsigned long long)nx_commands);
    svcExitProcess();
}

typedef struct {
    DkMemBlock arena, commands;
    uint8_t *cpu;
    DkGpuAddr gpu;
    uint32_t used, flushed, cmd_used;
    DkFence fence;   /* signalled again by each submit: only the frame's last one matters */
    int fenced;
} Frame;
static Frame frames[FRAMES];
static Frame *frame;
static unsigned frame_index;
static uint64_t frame_count;

static DkShader shader_vertex, shader_mesh, shader_pixel, shader_dual, shader_fullscreen, shader_blit,
                shader_upload, shader_grow, shader_convert, shader_present, shader_present_direct, shader_zread;

static void debug_callback(void *user, const char *context, DkResult result, const char *message)
{
    (void)user;
    nx_logf("deko3d: %s: %d: %s", context ? context : "", (int)result, message ? message : "");
    if (result != DkResult_Success) svcSleepThread(100000000);
}

/* ---- the image heap: first fit; a freed block waits until the GPU can't be using it ---- */
typedef struct Block { uint32_t offset, size; struct Block *next; } Block;
static Block *free_blocks;
static volatile uint32_t heap_used;
int dhdr_nx_heap_low(void) { return heap_used > HEAP_SIZE - HEAP_LOW; }
typedef struct Retired { uint32_t offset, size; int descriptor; uint64_t frame; struct Retired *next; } Retired;
static Retired *retired;
static int free_descriptors[IMAGE_DESCRIPTORS], free_descriptor_count;

static uint32_t heap_alloc(uint32_t size, uint32_t align)
{
    for (Block **p = &free_blocks; *p; p = &(*p)->next) {
        Block *b = *p;
        uint32_t at = (b->offset + align - 1) & ~(align - 1), skip = at - b->offset;
        if (b->size < skip + size) continue;
        uint32_t rest = b->size - skip - size, after = at + size;
        heap_used += size;
        if (skip) {
            b->size = skip;
            if (rest) {
                Block *n = malloc(sizeof *n);
                *n = (Block){after, rest, b->next};
                b->next = n;
            }
        } else if (rest) {
            b->offset = after, b->size = rest;
        } else {
            *p = b->next;
            free(b);
        }
        return at;
    }
    return UINT32_MAX;
}
static void heap_free(uint32_t offset, uint32_t size)
{
    heap_used -= size;
    Block **p = &free_blocks;
    while (*p && (*p)->offset < offset) p = &(*p)->next;
    Block *n = malloc(sizeof *n);
    *n = (Block){offset, size, *p};
    *p = n;
    if (n->next && n->offset + n->size == n->next->offset) {
        Block *m = n->next;
        n->size += m->size, n->next = m->next;
        free(m);
    }
    Block *before = free_blocks;
    while (before && before->next != n) before = before->next;
    if (before && before->offset + before->size == n->offset) {
        before->size += n->size, before->next = n->next;
        free(n);
    }
}
static void retire(uint32_t offset, uint32_t size, int descriptor)
{
    Retired *r = malloc(sizeof *r);
    mutexLock(&lock);
    /* one more than the fences need: another thread may read frame_count just as a frame begins */
    *r = (Retired){offset, size, descriptor, frame_count + FRAMES + 1, retired};
    retired = r;
    mutexUnlock(&lock);
}
static void retired_pass(void)
{
    mutexLock(&lock);
    for (Retired **p = &retired; *p;) {
        Retired *r = *p;
        if (r->frame > frame_count) { p = &r->next; continue; }
        *p = r->next;
        if (r->size) heap_free(r->offset, r->size);
        if (r->descriptor >= 0) free_descriptors[free_descriptor_count++] = r->descriptor;
        free(r);
    }
    mutexUnlock(&lock);
}

/* ---- images ---- */
typedef struct {
    DkImage image;
    DkImageView view;
    uint32_t offset, size;
    int sampled;            /* image descriptor index */
    uint32_t w, h;
    DkImageFormat format;
} Image;

/* A descriptor written since the GPU last dropped its cached ones (descriptors_sync). */
static int descriptors_dirty;

/* Lock held; one is free. */
static int descriptor_new(const DkImageView *view)
{
    int at = free_descriptors[--free_descriptor_count];
    dkImageDescriptorInitialize(&image_descriptors[at], view, false, false);
    __atomic_store_n(&descriptors_dirty, 1, __ATOMIC_RELEASE);
    return at;
}

/* deko3d 0.5.0: with 6 to 8 rows (blocks) in level 0, the layout and the descriptor disagree about the
   tile height, and the second column of blocks lands on the next level (halo3-nx HowToDeko3d, section 6,
   item 3; WWHD's image_tile_size_fix). Asking for the shrunk tile makes them agree. */
static void tile_size_fix(DkImageLayoutMaker *m, uint32_t rows)
{
    uint32_t gobs = (rows + rows / 2 + 7) / 8;   /* deko3d's pickTileSize */
    uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0, shrunk = tile;
    while (shrunk && (8u << (shrunk - 1)) >= rows) shrunk--;   /* its level 0 shrink (calcLevelOffset) */
    if (shrunk == tile) return;
    m->flags |= DkImageFlags_CustomTileSize;
    m->tileSize = (DkTileSize)shrunk;
}

/* Thread-safe. Contents zero, as WebGPU's are (a texture about to be uploaded needs no zeroing). */
static int image_make_with(Image *t, uint32_t w, uint32_t h, DkImageFormat format, uint32_t flags, uint32_t levels, int zero)
{
    if (!w || !h) return 0;   /* the layout call would end the process */
    DkImageLayoutMaker maker;
    DkImageLayout layout;
    dkImageLayoutMakerDefaults(&maker, device);
    maker.flags = flags;
    maker.format = format;
    maker.dimensions[0] = w, maker.dimensions[1] = h;
    maker.mipLevels = levels;
    tile_size_fix(&maker, h);
    dkImageLayoutInitialize(&layout, &maker);
    uint64_t size = dkImageLayoutGetSize(&layout);
    uint32_t align = dkImageLayoutGetAlignment(&layout);
    mutexLock(&lock);
    uint32_t offset = size > UINT32_MAX || !free_descriptor_count ? UINT32_MAX : heap_alloc((uint32_t)size, align);
    if (offset == UINT32_MAX) {
        uint32_t descriptors = (uint32_t)free_descriptor_count;
        mutexUnlock(&lock);
        static int told;
        if (!told++) nx_logf("gs native: no room for a %u x %u image (heap %u MiB used, %u descriptors free)", w, h, heap_used >> 20, descriptors);
        return 0;
    }
    memset(t, 0, sizeof *t);
    t->offset = offset, t->size = (uint32_t)size, t->w = w, t->h = h, t->format = format;
    if (zero) memset((uint8_t *)dkMemBlockGetCpuAddr(heap_block) + offset, 0, (size_t)size);
    dkImageInitialize(&t->image, &layout, heap_block, offset);
    dkImageViewDefaults(&t->view, &t->image);
    t->sampled = descriptor_new(&t->view);
    mutexUnlock(&lock);
    return 1;
}
static int image_make(Image *t, uint32_t w, uint32_t h, DkImageFormat format, uint32_t flags, uint32_t levels)
{
    return image_make_with(t, w, h, format, flags, levels, 1);
}
static void image_drop(Image *t)
{
    if (!t->size) return;
    retire(t->offset, t->size, t->sampled);
    t->size = 0;
}

#define SURFACE_FLAGS (DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | DkImageFlags_BlockLinear)

/* ---- a frame's memory ---- */
static void add_commands(void *user, DkCmdBuf c, size_t minimum)
{
    (void)user;
    uint32_t need = (uint32_t)(minimum + 256 > CMD_CHUNK ? (minimum + 256 + 0xFFF) & ~0xFFFu : CMD_CHUNK);
    if (frame->cmd_used + need > CMD_SIZE) {
        nx_log("gs native: out of command memory");
        svcExitProcess();
    }
    /* a little kept back: dkCmdBufBindVtxBuffers (0.5.0) writes a word more than it reserves */
    dkCmdBufAddMemory(c, frame->commands, frame->cmd_used, need - 64);
    frame->cmd_used += need;
}

/* Bytes in this frame's arena, or a GPU address of DK_GPU_ADDR_INVALID when it is full. */
static DkGpuAddr arena(uint32_t bytes, uint32_t align, void **cpu)
{
    uint32_t at = (frame->used + align - 1) & ~(align - 1);
    if (at + bytes > ARENA_SIZE) {
        nx_log("gs native: out of frame memory");
        return DK_GPU_ADDR_INVALID;
    }
    frame->used = at + bytes;
    *cpu = frame->cpu + at;
    return frame->gpu + at;
}
static DkGpuAddr constants_of(const void *data, uint32_t bytes)
{
    void *cpu;
    DkGpuAddr gpu = arena(bytes, DK_UNIFORM_BUF_ALIGNMENT, &cpu);
    if (gpu != DK_GPU_ADDR_INVALID) memcpy(cpu, data, bytes);
    return gpu;
}

static void frame_begin(void)
{
    frame_index = (frame_index + 1) % FRAMES;
    frame = &frames[frame_index];
    nx_render_stage = "waiting for the GPU";
    queue_check("a frame");
    if (frame->fenced) {
        uint64_t start = now_ns();
        while (dkFenceWait(&frame->fence, 2000000000) != DkResult_Success) {
            queue_check("a frame's fence");
            nx_logf("gs native: the GPU has not finished frame %llu", (unsigned long long)frame_count);
        }
        uint64_t waited = now_ns() - start;
        nx_t_gpuwait_ns += waited;
        static unsigned told;
        if (waited > 45000000 && told < 40) {
            told++;
            nx_logf("nx: GPU wait %.1f ms at frame %llu, presents %llu, heap %u MiB", waited / 1e6, (unsigned long long)frame_count,
                    (unsigned long long)nx_presents, heap_used >> 20);
        }
    }
    nx_render_stage = "recording";
    frame->fenced = 0;
    frame->used = frame->flushed = frame->cmd_used = 0;
    frame_count++;
    retired_pass();
    /* 0.5.0's clear rewinds to the last chunk added, the frame before's, still in flight: this
       frame's own memory goes in at once */
    dkCmdBufClear(cmd);
    add_commands(NULL, cmd, 0);
}

/* Everything recorded so far goes to the GPU; the frame's fence follows it. */
static void submit(void)
{
    uint64_t start = now_ns();
    queue_check("a submit");
    if (frame->used > frame->flushed) {
        dkMemBlockFlushCpuCache(frame->arena, frame->flushed, frame->used - frame->flushed);
        frame->flushed = frame->used;
    }
    DkCmdList list = dkCmdBufFinishList(cmd);
    dkQueueSubmitCommands(queue, list);
    dkQueueSignalFence(queue, &frame->fence, true);
    frame->fenced = 1;
    dkQueueFlush(queue);  /* the fence's own flush is the GPU's caches, not the queue */
    nx_t_submit_ns += now_ns() - start;
}

/* ---- the device's creation ---- */
static int shader_load(DkShader *s, const unsigned char *dksh, size_t bytes, uint32_t *at)
{
    DkShaderMaker maker;
    if (*at + bytes > CODE_SIZE - DK_SHADER_CODE_UNUSABLE_SIZE) return 0;   /* the block's end can't hold code */
    memcpy((uint8_t *)dkMemBlockGetCpuAddr(code_block) + *at, dksh, bytes);
    dkShaderMakerDefaults(&maker, code_block, *at);
    dkShaderInitialize(s, &maker);
    *at = (uint32_t)((*at + bytes + DK_SHADER_CODE_ALIGNMENT - 1) & ~(size_t)(DK_SHADER_CODE_ALIGNMENT - 1));
    return dkShaderIsValid(s);
}

static void inverse_table(uint32_t *inverse)
{
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            uint32_t bx = x / 16, by = y / 8;
            uint32_t block = (by & 1) | (bx & 1) << 1 | (by & 2) << 1 | (bx & 2) << 2 | (by & 4) << 2;
            uint32_t column = (x & 1) << 1 | (x & 2) << 2 | (x & 4) << 2 | (x & 8) >> 3 | (y & 1) << 2 | (y & 6) << 4;
            inverse[4096 + block * 128 + column] = x | y << 8;
        }
    for (uint32_t layout = 0; layout < 2; layout++)
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 64; x++) {
                uint32_t bx = x / 8, by = y / 8;
                uint32_t block = (bx & 1) | (by & 1) << 1 | (bx & 2) << 1 | (by & 2) << 2 | (bx & 4) << 2;
                uint32_t column = (x & 1) | (x & 6) << 1 | (y & 1) << 1 | (y & 6) << 3;
                if (layout == 1) block ^= 0x18;  /* PSMZ32 */
                inverse[layout * 2048 + ((block * 2048 + column * 32) >> 5 & 2047)] = x | y << 8;
            }
}

static DkMemBlock block_make(uint32_t size, uint32_t flags)
{
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device, (size + 0xFFF) & ~0xFFFu);
    maker.flags = flags;
    return dkMemBlockCreate(&maker);
}

static Image dummy, dummy_depth;
static DhdrTexture *inverse_texture;

int dhdr_device_init(const DhdrDeviceDesc *d)
{
    (void)d;
    if (ready) return ready > 0;
    mutexInit(&lock);
    strict_barriers = getenv("DHD_NX_BARRIERS") != NULL;
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = debug_callback;
    device = dkDeviceCreate(&dm);
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute;
    qm.commandMemorySize = 1 << 20;
    qm.flushThreshold = 1 << 17;
    queue = dkQueueCreate(&qm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, device);
    cm.cbAddMem = add_commands;
    cmd = dkCmdBufCreate(&cm);

    code_block = block_make(CODE_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    heap_block = block_make(HEAP_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    uint32_t descriptors = IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor) + SAMPLER_DESCRIPTORS * sizeof(DkSamplerDescriptor);
    descriptor_block = block_make(descriptors, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    /* read by the CPU each field: cached, each read invalidated after its fence (see zread_take) */
    zread_block = block_make(ZREAD_SLOTS * ZREAD_BYTES, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached);
    dkMemBlockFlushCpuCache(zread_block, 0, ZREAD_SLOTS * ZREAD_BYTES);
    for (int i = 0; i < FRAMES; i++) {
        frames[i].arena = block_make(ARENA_SIZE, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached);
        frames[i].commands = block_make(CMD_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        frames[i].cpu = dkMemBlockGetCpuAddr(frames[i].arena);
        frames[i].gpu = dkMemBlockGetGpuAddr(frames[i].arena);
    }
    frame = &frames[0];

    free_blocks = malloc(sizeof *free_blocks);
    *free_blocks = (Block){0, HEAP_SIZE, NULL};
    for (int i = IMAGE_DESCRIPTORS - 1; i >= 0; i--) free_descriptors[free_descriptor_count++] = i;

    image_descriptors = dkMemBlockGetCpuAddr(descriptor_block);
    image_set = dkMemBlockGetGpuAddr(descriptor_block);
    sampler_descriptors = (DkSamplerDescriptor *)(image_descriptors + IMAGE_DESCRIPTORS);
    sampler_set = image_set + IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor);
    DkSampler s;
    dkSamplerDefaults(&s);
    s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = DkWrapMode_ClampToEdge;
    dkSamplerDescriptorInitialize(&sampler_descriptors[SAMPLER_NEAREST], &s);
    s.minFilter = s.magFilter = DkFilter_Linear;
    dkSamplerDescriptorInitialize(&sampler_descriptors[SAMPLER_LINEAR], &s);
    for (int i = 0; i < 8; i++) {  /* bilinear | U clamped << 1 | V clamped << 2 (shared.rs) */
        dkSamplerDefaults(&s);
        s.wrapMode[0] = i & 2 ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
        s.wrapMode[1] = i & 4 ? DkWrapMode_ClampToEdge : DkWrapMode_Repeat;
        s.wrapMode[2] = DkWrapMode_ClampToEdge;
        s.magFilter = i & 1 ? DkFilter_Linear : DkFilter_Nearest;
        s.minFilter = DkFilter_Linear;
        s.mipFilter = DkMipFilter_Linear;
        s.maxAnisotropy = i & 1 ? 16.0f : 1.0f;
        dkSamplerDescriptorInitialize(&sampler_descriptors[SAMPLER_REPLACE + i], &s);
    }
    uint32_t at = 0;
#define LOAD(var, name) shader_load(&var, dksh_##name, sizeof dksh_##name, &at)
    int ok = LOAD(shader_vertex, native_vertex) && LOAD(shader_mesh, native_mesh_vertex) &&
             LOAD(shader_pixel, native_pixel) && LOAD(shader_dual, native_pixel_dual) &&
             LOAD(shader_fullscreen, fullscreen_vertex) && LOAD(shader_blit, native_blit) &&
             LOAD(shader_upload, native_upload_alpha) && LOAD(shader_grow, native_grow_depth) && LOAD(shader_convert, native_convert) &&
             LOAD(shader_present, present_pixel) && LOAD(shader_present_direct, present_direct) && LOAD(shader_zread, zread);
    if (!ok) {
        nx_log("gs native: a shader did not load");
        ready = -1;
        return 0;
    }
    image_make(&dummy, 1, 1, DkImageFormat_RGBA8_Unorm, 0, 1);
    image_make(&dummy_depth, 1, 1, DkImageFormat_ZF32, DkImageFlags_UsageRender, 1);
    adapter = "Tegra X1 (deko3d)";
    ready = 1;
    return 1;
}
const char *dhdr_adapter_name(void) { return adapter; }
uint32_t dhdr_device_features(void) { return ready > 0 ? DHDR_FEAT_DUAL_SOURCE | DHDR_FEAT_RENDER_PIPELINES : 0; }
const char *dhdr_version(void) { return "dhd_render nx 1.0 deko3d"; }
uint32_t dhdr_device_max_texture(void) { return ready > 0 ? 16384 : 0; }
/* No GPU timestamps here yet: the timeline and perf_log read nothing (they need 4 values). */
uint32_t dhdr_perf_read(uint64_t *out, uint32_t count) { (void)out; (void)count; return 0; }
/* What main's renderer gained since 0.7.6 and this one has not yet (finding 270): deko3d reports
   no device loss here; no pipeline list to prebuild; no surface read back (Poster Maker's frame:
   nothing to read). VU1 mesh draws (Native VU) are drawn: native_mesh_vertex and the mesh key. */
int dhdr_device_lost(void) { return 0; }
int dhdr_native_meshes(void) { return ready > 0; }
void dhdr_native_pipeline_list(const char *path) { (void)path; }
int dhdr_native_surface_read_begin(DhdrNative *n, uint32_t index, const uint32_t rect[4], uint32_t size[2])
{
    (void)n; (void)index; (void)rect; (void)size;
    return 0;
}
int dhdr_native_surface_read_finish(DhdrNative *n, uint32_t *pixels) { (void)n; (void)pixels; return 0; }

/* ---- textures: reference counted, made on any thread, uploaded on the render thread ---- */
struct DhdrTexture {
    Image image;
    int refs, levels;
    uint8_t *pixels;           /* waiting for the render thread; NULL once uploaded */
    size_t pixel_bytes;
    struct DhdrTexture *next_upload;
};
static DhdrTexture *uploads;

static DhdrTexture *texture_new(const void *rgba, size_t bytes, uint32_t w, uint32_t h, uint32_t levels)
{
    DhdrTexture *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    if (!(t->pixels = malloc(bytes)) || !image_make_with(&t->image, w, h, DkImageFormat_RGBA8_Unorm, 0, levels, 0)) {
        free(t->pixels);
        free(t);
        return NULL;
    }
    memcpy(t->pixels, rgba, bytes);
    t->pixel_bytes = bytes, t->refs = 1, t->levels = (int)levels;
    mutexLock(&lock);
    t->next_upload = uploads;
    uploads = t;
    t->refs++;   /* the list's own */
    mutexUnlock(&lock);
    return t;
}
DhdrTexture *dhdr_texture_create(const uint32_t *rgba, uint32_t w, uint32_t h)
{
    return w && h ? texture_new(rgba, (size_t)w * h * 4, w, h, 1) : NULL;
}
DhdrTexture *dhdr_replace_create(const uint8_t *rgba_mips, uint32_t w, uint32_t h, uint32_t levels)
{
    size_t bytes = 0;
    uint32_t lw = w, lh = h;
    for (uint32_t i = 0; i < levels; i++) {
        bytes += (size_t)lw * lh * 4;
        lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1;
    }
    return w && h && levels ? texture_new(rgba_mips, bytes, w, h, levels) : NULL;
}
void dhdr_texture_retain(DhdrTexture *t) { if (t) __atomic_add_fetch(&t->refs, 1, __ATOMIC_RELAXED); }
void dhdr_texture_release(DhdrTexture *t)
{
    if (!t || __atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL)) return;
    image_drop(&t->image);
    free(t->pixels);
    free(t);
}

/* The conversions' table, x in red and y in green (nx_shaders.py's nx_inverse). */
static int inverse_texture_make(void)
{
    uint32_t *inverse = calloc(INVERSE_WORDS, 4);
    if (!inverse) return 0;
    inverse_table(inverse);
    inverse_texture = dhdr_texture_create(inverse, INVERSE_W, INVERSE_H);
    free(inverse);
    return inverse_texture != NULL;
}

static void copy_rect(DkImageRect *r, uint32_t x, uint32_t y, uint32_t w, uint32_t h) { *r = (DkImageRect){x, y, 0, w, h, 1}; }

/* The waiting textures' pixels, at a picture's start. What doesn't fit this frame waits for the next. */
static void uploads_pass(void)
{
    mutexLock(&lock);
    DhdrTexture *list = uploads;
    uploads = NULL;
    mutexUnlock(&lock);
    int any = 0;
    for (DhdrTexture *t = list, *next; t; t = next) {
        next = t->next_upload;
        void *cpu;
        DkGpuAddr gpu = frame->used + t->pixel_bytes + (32 << 20) < ARENA_SIZE ? arena((uint32_t)t->pixel_bytes, 64, &cpu) : DK_GPU_ADDR_INVALID;
        if (gpu == DK_GPU_ADDR_INVALID) {
            mutexLock(&lock);
            t->next_upload = uploads;
            uploads = t;
            mutexUnlock(&lock);
            continue;
        }
        memcpy(cpu, t->pixels, t->pixel_bytes);
        uint32_t lw = t->image.w, lh = t->image.h;
        for (int level = 0; level < t->levels; level++) {
            DkImageView view = t->image.view;
            view.mipLevelOffset = (uint8_t)level, view.mipLevelCount = 1;
            DkCopyBuf from = {gpu, lw * 4, lh};
            DkImageRect rect;
            copy_rect(&rect, 0, 0, lw, lh);
            dkCmdBufCopyBufferToImage(cmd, &from, &view, &rect, 0);
            gpu += (DkGpuAddr)lw * lh * 4;
            lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1;
        }
        free(t->pixels);
        t->pixels = NULL;
        any = 1;
        dhdr_texture_release(t);
    }
    if (any) barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
}

/* ---- the native renderer's objects ---- */
typedef struct { Image image; uint32_t scale, size[2]; int depth, exists; } Surface;
typedef struct { uint32_t index; Image old; uint32_t size[2]; int depth; } Grow;

struct DhdrNative {
    Surface surfaces[SURFACES];
    Grow grows[SURFACES];
    int grow_count;
    Image scratch, import, display;
    int has_scratch, has_import, has_display;
    uint32_t scratch_size[2], display_w, display_h;
    Image converted[CONVERTS];
    int has_converted[CONVERTS];
    uint32_t present_sampler;
    /* The last picture, shown straight from its display surface (finding 247; -1: the display image
       holds it): that surface, the picture's rectangle there (x, y, width, rows read) and its rows on
       screen (twice the rows read when doubled). The display image doesn't hold it until materialize. */
    int direct_surface;
    uint32_t direct_rect[4], direct_h;
    /* the open draw pass; -2: none */
    int target, depth;
    int indexed;
    DkGpuAddr vertices, indices;
    uint32_t vertex_bytes;
    /* draw state last set in the command buffer; invalid after a pass change where noted */
    uint32_t bound_key;
    int bound_valid;
    int32_t pending_first, pending_count;
    DkGpuAddr constants_addr;
    DhdrNativeConstants constants_last;
    int constants_valid;
    int uniform_set;
    DkGpuAddr uniform_bound;
    int texture_bound;
    uint32_t scissor[4], viewport[2];
    uint32_t blend_set, blend_valid, scissor_valid, viewport_valid;
    const DkShader *rect_pixel;   /* a full-screen pass's state is bound, with this pixel shader */
    /* what a barrier would order (surfaces_sync): pixels written, or surfaces read, since the last one */
    int writes, sampled;
    int zcull_stale;              /* a Z image took new memory: ZCULL still describes the old contents */
    DhdrPictureResult drawn;
    /* Z probes */
    struct { DkFence fence; uint32_t rect[4]; uint64_t tag; int used, done; } zread[ZREAD_SLOTS];
};

static int inverse_texture_ready(void) { return inverse_texture || inverse_texture_make(); }

DhdrNative *dhdr_native_create(uint32_t flags)
{
    (void)flags;
    if (ready <= 0 || !inverse_texture_ready()) return NULL;
    DhdrNative *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->target = n->depth = -2;
    n->direct_surface = -1;
    n->present_sampler = SAMPLER_LINEAR;   /* Native::create's first options(0) */
    return n;
}
void dhdr_native_destroy(DhdrNative *n) { free(n); }
void dhdr_native_options(DhdrNative *n, uint32_t flags)
{
    n->present_sampler = flags & DHDR_NATIVE_POINT_FILTER ? SAMPLER_NEAREST : SAMPLER_LINEAR;
}

int dhdr_native_surface(DhdrNative *n, uint32_t index, uint32_t scale, uint32_t w, uint32_t h, uint32_t depth, uint32_t size_out[2])
{
    if (index >= SURFACES || !scale) return 0;
    Surface *s = &n->surfaces[index];
    if (s->exists && s->depth != (int)depth) {
        image_drop(&s->image);
        s->exists = 0;
        for (int i = 0; i < n->grow_count;) {
            if (n->grows[i].index != index) { i++; continue; }
            image_drop(&n->grows[i].old);
            n->grows[i] = n->grows[--n->grow_count];
        }
    }
    s->scale = scale, s->depth = (int)depth;
    if (s->exists && s->size[0] >= w && s->size[1] >= h) {
        size_out[0] = s->size[0], size_out[1] = s->size[1];
        return 1;
    }
    if (s->exists) {
        if (w < s->size[0]) w = s->size[0];
        if (h < s->size[1]) h = s->size[1];
    }
    Image made;
    if (!image_make(&made, w, h, depth ? DkImageFormat_ZF32 : DkImageFormat_RGBA8_Unorm,
                    depth ? DkImageFlags_UsageRender : SURFACE_FLAGS, 1))
        return 0;
    if (s->exists) {
        int pending = 0;
        for (int i = 0; i < n->grow_count; i++) pending |= n->grows[i].index == index;
        if (pending) image_drop(&s->image);  /* grown twice: the middle one holds nothing yet */
        else n->grows[n->grow_count++] = (Grow){index, s->image, {s->size[0], s->size[1]}, (int)depth};
    }
    s->image = made;
    dkImageViewDefaults(&s->image.view, &s->image.image);  /* the copy's own: a view points at its image */
    if (depth) n->zcull_stale = 1;
    s->exists = 1;
    s->size[0] = w, s->size[1] = h;
    size_out[0] = w, size_out[1] = h;
    return 1;
}

/* ---- passes ---- */
/* Before work that samples images: the GPU caches image descriptors, and a slot freed and given to a
   new image (descriptor_new) still reads as the old one until they are invalidated (finding 270: the
   menus' textures came out as pieces of other images). */
static void descriptors_sync(void)
{
    if (!__atomic_exchange_n(&descriptors_dirty, 0, __ATOMIC_ACQ_REL)) return;
    barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
}

static void flush_draw(DhdrNative *n)
{
    if (!n->pending_count) return;
    descriptors_sync();
    if (n->indexed) dkCmdBufDrawIndexed(cmd, DkPrimitive_Triangles, (uint32_t)n->pending_count, 1, (uint32_t)n->pending_first, 0, 0);
    else dkCmdBufDraw(cmd, DkPrimitive_Triangles, (uint32_t)n->pending_count, 1, (uint32_t)n->pending_first, 0);
    n->pending_count = 0;
}
/* Before reading what passes wrote or writing what they read, and anything a copy touches. A pass
   that follows a pass with neither (the common case) needs none: the 3D engine keeps its own order. */
static void surfaces_sync(DhdrNative *n)
{
    flush_draw(n);
    if (!n->writes && !n->sampled) return;
    barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
    n->writes = n->sampled = 0;
}
/* After a copy-engine write, which the 3D engine doesn't order itself against. */
static void copied(DhdrNative *n)
{
    barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
    n->writes = n->sampled = 0;
}
static void pass_end(DhdrNative *n)
{
    flush_draw(n);
    if (n->target == -2) return;
    n->target = n->depth = -2;
    n->writes = 1;
    if (strict_barriers) surfaces_sync(n);
}

static const DkVtxAttribState vertex_attributes[3] = {
    {.bufferId = 0, .isFixed = 0, .offset = 0, .size = DkVtxAttribSize_3x32, .type = DkVtxAttribType_Float, .isBgra = 0},
    {.bufferId = 0, .isFixed = 0, .offset = 12, .size = DkVtxAttribSize_4x32, .type = DkVtxAttribType_Float, .isBgra = 0},
    {.bufferId = 0, .isFixed = 0, .offset = 28, .size = DkVtxAttribSize_4x8, .type = DkVtxAttribType_Uint, .isBgra = 0},
};
/* A translated VU1 mesh draw's (flag 4096, native_core.inc's native_mesh): the same 32 bytes as
   gs_mesh.h's GsMeshVertex, the clip-space position, u v q, the colour's bytes (pipelines.rs's
   MESH_ATTRIBUTES), for native.wgsl's native_mesh_vertex. */
static const DkVtxAttribState mesh_attributes[3] = {
    {.bufferId = 0, .isFixed = 0, .offset = 0, .size = DkVtxAttribSize_4x32, .type = DkVtxAttribType_Float, .isBgra = 0},
    {.bufferId = 0, .isFixed = 0, .offset = 16, .size = DkVtxAttribSize_3x32, .type = DkVtxAttribType_Float, .isBgra = 0},
    {.bufferId = 0, .isFixed = 0, .offset = 28, .size = DkVtxAttribSize_4x8, .type = DkVtxAttribType_Uint, .isBgra = 0},
};
static const DkVtxBufferState vertex_buffer = {32, 0};

/* The draw key's mesh bits (render.rs's MESH and MESH_TWO_SIDED): a mesh draw culls back faces
   unless its loop draws both sides (flag 8192, finding 167). */
enum { KEY_MESH = 1u << 24, KEY_TWO_SIDED = 1u << 25 };
static int raster_culls;

/* The rasterizer: back faces culled for a one-sided mesh, else none (every other draw and pass).
   A mesh's back faces have a positive area in GS pixels (y down), clockwise in clip space
   (vu1_mesh.inc); deko3d's CCW front face is wgpu's here, both judged in clip space (y up). */
static void raster_bind(int cull)
{
    if (raster_culls == cull) return;
    DkRasterizerState raster;
    dkRasterizerStateDefaults(&raster);
    raster.cullMode = cull ? DkFace_Back : DkFace_None;
    dkCmdBufBindRasterizerState(cmd, &raster);
    raster_culls = cull;
}

/* A full-screen pass's state: no blending, no depth, all channels written. A run of them (IMPORT pages)
   binds it once; the draw state's record is out of date either way. */
static void fullscreen_state(DhdrNative *n, const DkShader *pixel)
{
    n->bound_valid = n->blend_valid = n->scissor_valid = n->viewport_valid = 0;
    n->uniform_set = n->texture_bound = 0;
    raster_bind(0);
    if (n->rect_pixel != pixel) {
        DkShader const *shaders[2] = {&shader_fullscreen, pixel};
        dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
    }
    if (!n->rect_pixel) {
        DkColorState color;
        dkColorStateDefaults(&color);
        dkCmdBufBindColorState(cmd, &color);
        DkColorWriteState writes;
        dkColorWriteStateDefaults(&writes);
        dkCmdBufBindColorWriteState(cmd, &writes);
        DkDepthStencilState depth;
        dkDepthStencilStateDefaults(&depth);
        depth.depthTestEnable = 0, depth.depthWriteEnable = 0;
        dkCmdBufBindDepthStencilState(cmd, &depth);
    }
    n->rect_pixel = pixel;
}

/* A full-screen triangle's pass (surfaces.rs rect_pass): `map` its constants, cut to (x, y, w, h). */
static void rect_pass(DhdrNative *n, const DkImageView *target, const uint32_t size[2], const DkShader *pixel, const float map[4],
                      const Image *t0, const Image *t1, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (x >= size[0] || y >= size[1]) return;
    if (w > size[0] - x) w = size[0] - x;
    if (h > size[1] - y) h = size[1] - y;
    if (!w || !h) return;
    pass_end(n);
    surfaces_sync(n);
    DkGpuAddr uniform = constants_of(map, 16);
    if (uniform == DK_GPU_ADDR_INVALID) return;
    dkCmdBufBindRenderTarget(cmd, target, NULL);
    fullscreen_state(n, pixel);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, uniform, 16);
    dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle((uint32_t)t0->sampled, SAMPLER_NEAREST));
    if (t1) dkCmdBufBindTexture(cmd, DkStage_Fragment, 1, dkMakeTextureHandle((uint32_t)t1->sampled, SAMPLER_NEAREST));
    DkViewport viewport = {(float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f};
    dkCmdBufSetViewports(cmd, 0, &viewport, 1);
    DkScissor scissor = {x, y, w, h};
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);
    descriptors_sync();
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    n->writes = n->sampled = 1;
    if (strict_barriers) surfaces_sync(n);
}

/* surfaces.rs blit: a w x h GS-pixel rectangle of a texture at scale `from` into surface t. */
static void blit(DhdrNative *n, const Image *source, uint32_t from, int sx, int sy, Surface *t, int x, int y, int w, int h)
{
    uint32_t tt = t->scale;
    if (!t->exists || !tt) return;
    int tw = (int)(t->size[0] / tt), th = (int)(t->size[1] / tt);
    if (x + w > tw) w = tw - x;
    if (y + h > th) h = th - y;
    if (w <= 0 || h <= 0 || x < 0 || y < 0) return;
    float map[4] = {(float)(sx - x), (float)(sy - y), (float)tt, (float)from};
    rect_pass(n, &t->image.view, t->size, &shader_blit, map, source, NULL, (uint32_t)x * tt, (uint32_t)y * tt, (uint32_t)w * tt, (uint32_t)h * tt);
}

static void copy_box(const Image *from, const uint32_t from_size[2], uint32_t sx, uint32_t sy, const Image *to, const uint32_t to_size[2],
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (sx >= from_size[0] || sy >= from_size[1] || x >= to_size[0] || y >= to_size[1]) return;
    if (w > from_size[0] - sx) w = from_size[0] - sx;
    if (h > from_size[1] - sy) h = from_size[1] - sy;
    if (w > to_size[0] - x) w = to_size[0] - x;
    if (h > to_size[1] - y) h = to_size[1] - y;
    if (!w || !h) return;
    DkImageRect a, b;
    copy_rect(&a, sx, sy, w, h);
    copy_rect(&b, x, y, w, h);
    dkCmdBufCopyImage(cmd, &from->view, &a, &to->view, &b, 0);
}

static int scratch_ready(DhdrNative *n, uint32_t w, uint32_t h)
{
    if (n->has_scratch && n->scratch_size[0] >= w && n->scratch_size[1] >= h) return 1;
    if (w < n->scratch_size[0]) w = n->scratch_size[0];
    if (h < n->scratch_size[1]) h = n->scratch_size[1];
    if (n->has_scratch) image_drop(&n->scratch);
    n->has_scratch = image_make(&n->scratch, w, h, DkImageFormat_RGBA8_Unorm, DkImageFlags_Usage2DEngine | DkImageFlags_UsageRender, 1);
    n->scratch_size[0] = n->has_scratch ? w : 0, n->scratch_size[1] = n->has_scratch ? h : 0;
    return n->has_scratch;
}
static int import_ready(DhdrNative *n)
{
    if (!n->has_import) {
        n->has_import = image_make(&n->import, IMPORT_SIDE, IMPORT_SIDE, DkImageFormat_RGBA8_Unorm, 0, 1);
    }
    return n->has_import;
}

/* w x h pixels (rows of `stride` pixels) into an image at (x, y). */
static void upload(const Image *to, const uint32_t *pixels, uint32_t stride, uint32_t rows, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    void *cpu;
    uint32_t bytes = stride * rows * 4;
    DkGpuAddr gpu = arena(bytes, 64, &cpu);
    if (gpu == DK_GPU_ADDR_INVALID) return;
    memcpy(cpu, pixels, bytes);
    DkCopyBuf from = {gpu, stride * 4, rows};
    DkImageRect rect;
    copy_rect(&rect, x, y, w, h);
    dkCmdBufCopyBufferToImage(cmd, &from, &to->view, &rect, 0);
}

static void surface_copy(DhdrNative *n, const DhdrCommand *k)
{
    if (k->source < 0 || k->source >= SURFACES || k->target < 0 || k->target >= SURFACES) return;
    Surface *s = &n->surfaces[k->source], *t = &n->surfaces[k->target];
    if (!s->exists || !t->exists || s->depth || t->depth) return;
    if (s->scale != t->scale) {
        blit(n, &s->image, s->scale, k->sx, k->sy, t, k->x, k->y, k->w, k->h);
        return;
    }
    int ss = (int)s->scale, w = k->w, h = k->h;
    if (!ss) return;
    int sw = (int)s->size[0] / ss, sh = (int)s->size[1] / ss, tw = (int)t->size[0] / ss, th = (int)t->size[1] / ss;
    if (k->sx + w > sw) w = sw - k->sx;
    if (k->sy + h > sh) h = sh - k->sy;
    if (k->x + w > tw) w = tw - k->x;
    if (k->y + h > th) h = th - k->y;
    if (w <= 0 || h <= 0 || k->sx < 0 || k->sy < 0 || k->x < 0 || k->y < 0) return;
    n->drawn.copies++;
    n->drawn.copy_px += (uint64_t)w * (uint64_t)h;
    uint32_t bx = (uint32_t)(k->sx * ss), by = (uint32_t)(k->sy * ss), bw = (uint32_t)(w * ss), bh = (uint32_t)(h * ss);
    uint32_t dx = (uint32_t)(k->x * ss), dy = (uint32_t)(k->y * ss);
    surfaces_sync(n);
    if (k->source == k->target) {
        if (!scratch_ready(n, bx + bw, by + bh)) return;
        copy_box(&s->image, s->size, bx, by, &n->scratch, n->scratch_size, bx, by, bw, bh);
        barrier(DkBarrier_Fragments, 0);
        copy_box(&n->scratch, n->scratch_size, bx, by, &s->image, s->size, dx, dy, bw, bh);
    } else {
        copy_box(&s->image, s->size, bx, by, &t->image, t->size, dx, dy, bw, bh);
    }
    copied(n);
}

static void surface_import(DhdrNative *n, const DhdrCommand *k)
{
    if (k->target < 0 || k->target >= SURFACES) return;
    Surface *s = &n->surfaces[k->target];
    if (!s->exists || s->depth) return;
    if (s->scale == 1) {
        uint32_t x = (uint32_t)k->x, y = (uint32_t)k->y, w = (uint32_t)k->w, h = (uint32_t)k->h;
        if (x < s->size[0] && y < s->size[1] && k->pixels) {
            if (w > s->size[0] - x) w = s->size[0] - x;
            if (h > s->size[1] - y) h = s->size[1] - y;
            surfaces_sync(n);
            upload(&s->image, k->pixels, (uint32_t)k->w, (uint32_t)k->h, x, y, w, h);
            copied(n);
        }
    } else if (k->w <= IMPORT_SIDE && k->h <= IMPORT_SIDE && k->pixels && import_ready(n)) {
        surfaces_sync(n);
        upload(&n->import, k->pixels, (uint32_t)k->w, (uint32_t)k->h, 0, 0, (uint32_t)k->w, (uint32_t)k->h);
        copied(n);
        blit(n, &n->import, 1, 0, 0, s, k->x, k->y, k->w, k->h);
    }
    n->drawn.imports++;
    n->drawn.import_px += (uint64_t)(int64_t)k->w * (uint64_t)(int64_t)k->h;
}

static void surface_alpha_upload(DhdrNative *n, const DhdrCommand *k)
{
    if (k->target < 0 || k->target >= SURFACES) return;
    Surface *s = &n->surfaces[k->target];
    uint32_t ss = s->scale, left = (uint32_t)k->x * ss, top = (uint32_t)k->y * ss;
    uint32_t right = (uint32_t)(k->x + 64) * ss, bottom = (uint32_t)(k->y + 32) * ss;
    if (!s->exists || s->depth || !k->pixels || !scratch_ready(n, right, bottom) || !import_ready(n)) return;
    surfaces_sync(n);
    copy_box(&s->image, s->size, left, top, &n->scratch, n->scratch_size, left, top, right - left, bottom - top);
    upload(&n->import, k->pixels, 64, 32, 0, 0, 64, 32);
    copied(n);
    float map[4] = {(float)k->x, (float)k->y, (float)ss, 0.0f};
    rect_pass(n, &s->image.view, s->size, &shader_upload, map, &n->import, &n->scratch, left, top, 64 * ss, 32 * ss);
}

/* surfaces.rs sample_target: the region a draw reads of its own target, into the scratch image. */
static int sample_target(DhdrNative *n, int index, const DhdrNativeConstants *c)
{
    Surface *s = &n->surfaces[index];
    uint32_t ss = s->scale;
    if (!ss) return 0;
    int x = c->tex_offset[0], y = c->tex_offset[1], w = (int)c->tex_size[0], h = (int)c->tex_size[1];
    int sw = (int)(s->size[0] / ss), sh = (int)(s->size[1] / ss);
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (x < 0 || y < 0) return 0;
    if (!scratch_ready(n, (uint32_t)(x + w) * ss, (uint32_t)(y + h) * ss)) return 0;
    if (w && h) {
        surfaces_sync(n);
        copy_box(&s->image, s->size, (uint32_t)x * ss, (uint32_t)y * ss, &n->scratch, n->scratch_size,
                 (uint32_t)x * ss, (uint32_t)y * ss, (uint32_t)w * ss, (uint32_t)h * ss);
        copied(n);
    }
    n->drawn.self_copies++;
    n->drawn.self_copy_px += (uint64_t)w * (uint64_t)h;
    return 1;
}

/* Grown surfaces' old contents, at the picture's start. */
static void surfaces_prepare(DhdrNative *n)
{
    for (int i = 0; i < n->grow_count; i++) {
        Grow *g = &n->grows[i];
        Surface *s = &n->surfaces[g->index];
        dkImageViewDefaults(&g->old.view, &g->old.image);  /* moved into grows[] by value */
        if (s->exists) {
            pass_end(n);
            surfaces_sync(n);
            if (g->depth) {
                dkCmdBufBindRenderTarget(cmd, NULL, &s->image.view);
                DkShader const *shaders[2] = {&shader_fullscreen, &shader_grow};
                dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
                raster_bind(0);
                DkDepthStencilState depth;
                dkDepthStencilStateDefaults(&depth);
                depth.depthTestEnable = 1, depth.depthWriteEnable = 1, depth.depthCompareOp = DkCompareOp_Always;
                dkCmdBufBindDepthStencilState(cmd, &depth);
                dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle((uint32_t)g->old.sampled, SAMPLER_NEAREST));
                DkViewport viewport = {0.0f, 0.0f, (float)g->size[0], (float)g->size[1], 0.0f, 1.0f};
                dkCmdBufSetViewports(cmd, 0, &viewport, 1);
                DkScissor scissor = {0, 0, g->size[0], g->size[1]};
                dkCmdBufSetScissors(cmd, 0, &scissor, 1);
                descriptors_sync();
                dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
                n->writes = n->sampled = 1;
                n->rect_pixel = NULL;
                n->zcull_stale = 1;   /* written by frag_depth, and the address is new */
            } else {
                copy_box(&g->old, g->size, 0, 0, &s->image, s->size, 0, 0, g->size[0], g->size[1]);
                copied(n);
            }
        }
        image_drop(&g->old);
    }
    n->grow_count = 0;
    n->bound_valid = n->blend_valid = n->scissor_valid = n->viewport_valid = n->uniform_set = n->texture_bound = 0;
    /* deko3d drops ZCULL only when the bound Z address changes; a new image may take the old address,
       with contents it was never told of */
    if (n->zcull_stale) {
        surfaces_sync(n);
        barrier(DkBarrier_None, DkInvalidateFlags_Zcull);
        n->zcull_stale = 0;
    }
}

/* ---- conversions (convert.rs) ---- */
/* DHD_NX_CONVERT_BATCH (on unless 0; finding 270 step 3, after finding 112): the conversions render()
   prepares together at a pass boundary go without a barrier between them. Each reads surfaces and
   writes its own image; after the first one's barrier nothing but those conversions is recorded, so
   none reads what another wrote, nor writes what a draw may still be reading. The draws that sample
   them wait once, after the last (render's reads_surface). Off: a barrier before each, as before. */
static int convert_batch(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("DHD_NX_CONVERT_BATCH");
        on = !(v && !strcmp(v, "0"));
        nx_logf("nx renderer: prepared conversions %s (DHD_NX_CONVERT_BATCH)", on ? "batched, one barrier" : "a barrier each");
    }
    return on;
}

/* Conversion i into its image. batched: it follows another of render's prepared run, with nothing
   recorded between (convert_batch): no barrier. */
static const Image *convert_run(DhdrNative *n, const DhdrPicture *p, int i, int batched)
{
    if (i < 0 || (uint32_t)i >= p->convert_count || i >= CONVERTS) return NULL;
    const DhdrConvert *v = &p->converts[i];
    uint32_t w = v->constants.size[0], h = v->constants.size[1];
    if (!w || !h) return NULL;
    DhdrConvertConstants constants = v->constants;
    constants.tex[3] = 0;
    const Image *colour[4], *depth[4];
    for (int k = 0; k < 4; k++) {
        colour[k] = &dummy, depth[k] = &dummy_depth;
        uint32_t scale = 1;
        if (v->surface[k] >= 0) {
            const Surface *s = v->surface[k] < SURFACES ? &n->surfaces[v->surface[k]] : NULL;
            if (!s || !s->exists) return NULL;
            if (s->depth) depth[k] = &s->image;
            else colour[k] = &s->image;
            scale = s->scale;
        }
        constants.tex[3] |= scale << (k * 8);
    }
    Image *out = &n->converted[i];
    if (!n->has_converted[i] || out->w != w || out->h != h) {
        if (n->has_converted[i]) image_drop(out);
        /* no zeroing: the pass writes every pixel */
        n->has_converted[i] = image_make_with(out, w, h, DkImageFormat_RGBA8_Unorm, SURFACE_FLAGS, 1, 0);
        if (!n->has_converted[i]) return NULL;
    }
    pass_end(n);
    if (!batched) surfaces_sync(n);
    DkGpuAddr uniform = constants_of(&constants, sizeof constants);
    if (uniform == DK_GPU_ADDR_INVALID) return NULL;
    const Image *base = v->base ? &v->base->image : &dummy;
    DkResHandle textures[10] = {
        dkMakeTextureHandle((uint32_t)inverse_texture->image.sampled, SAMPLER_NEAREST),
        dkMakeTextureHandle((uint32_t)base->sampled, SAMPLER_NEAREST),
        dkMakeTextureHandle((uint32_t)colour[0]->sampled, SAMPLER_NEAREST), dkMakeTextureHandle((uint32_t)colour[1]->sampled, SAMPLER_NEAREST),
        dkMakeTextureHandle((uint32_t)colour[2]->sampled, SAMPLER_NEAREST), dkMakeTextureHandle((uint32_t)colour[3]->sampled, SAMPLER_NEAREST),
        dkMakeTextureHandle((uint32_t)depth[0]->sampled, SAMPLER_NEAREST), dkMakeTextureHandle((uint32_t)depth[1]->sampled, SAMPLER_NEAREST),
        dkMakeTextureHandle((uint32_t)depth[2]->sampled, SAMPLER_NEAREST), dkMakeTextureHandle((uint32_t)depth[3]->sampled, SAMPLER_NEAREST),
    };
    dkCmdBufBindRenderTarget(cmd, &out->view, NULL);
    fullscreen_state(n, &shader_convert);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, uniform, (uint32_t)sizeof constants);
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, textures, 10);
    DkViewport viewport = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    dkCmdBufSetViewports(cmd, 0, &viewport, 1);
    DkScissor scissor = {0, 0, w, h};
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);
    descriptors_sync();
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    n->writes = n->sampled = 1;
    if (strict_barriers) surfaces_sync(n);
    return out;
}

/* ---- draws (render.rs draw_passes, with dual-source blending) ---- */
static DkBlendFactor blend_factor(uint32_t d3d, int dual)
{
    switch (d3d) {
    case 1: return DkBlendFactor_Zero;
    case 2: return DkBlendFactor_One;
    case 5: return dual ? DkBlendFactor_Src1Alpha : DkBlendFactor_SrcAlpha;
    case 6: return dual ? DkBlendFactor_InvSrc1Alpha : DkBlendFactor_InvSrcAlpha;
    case 7: return DkBlendFactor_DstAlpha;
    case 8: return DkBlendFactor_InvDstAlpha;
    case 14: return DkBlendFactor_ConstColor;
    case 15: return DkBlendFactor_InvConstColor;
    default: return DkBlendFactor_One;
    }
}
static DkBlendOp blend_op(uint32_t d3d)
{
    switch (d3d) {
    case 2: return DkBlendOp_Sub;
    case 3: return DkBlendOp_RevSub;
    case 4: return DkBlendOp_Min;
    case 5: return DkBlendOp_Max;
    default: return DkBlendOp_Add;
    }
}
static DkCompareOp depth_compare(uint32_t function)
{
    switch (function) {
    case 0: return DkCompareOp_Never;
    case 2: return DkCompareOp_Gequal;
    case 3: return DkCompareOp_Greater;
    case 4: return DkCompareOp_Equal;
    default: return DkCompareOp_Always;
    }
}

/* A key: blend_key | mask << 14 | depth << 18 | dual << 23 (pipelines.rs). */
static void set_key(DhdrNative *n, uint32_t key)
{
    if (n->bound_valid && n->bound_key == key) return;
    flush_draw(n);
    n->rect_pixel = NULL;
    uint32_t blend_key = key & 0x3FFF, mask = key >> 14 & 15, depth = key >> 18 & 31;
    int dual = key >> 23 & 1, blending = blend_key & 1, mesh = (key & KEY_MESH) != 0;
    if (!n->bound_valid || (n->bound_key >> 23 & 1) != (uint32_t)dual || ((n->bound_key ^ key) & KEY_MESH)) {
        DkShader const *shaders[2] = {mesh ? &shader_mesh : &shader_vertex, dual ? &shader_dual : &shader_pixel};
        dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
    }
    if (!n->bound_valid || ((n->bound_key ^ key) & KEY_MESH))
        dkCmdBufBindVtxAttribState(cmd, mesh ? mesh_attributes : vertex_attributes, 3);
    raster_bind(mesh && !(key & KEY_TWO_SIDED));
    if (!n->bound_valid || (n->bound_key & 0x3FFF) != blend_key || (n->bound_key >> 23 & 1) != (uint32_t)dual) {
        DkColorState color;
        dkColorStateDefaults(&color);
        dkColorStateSetBlendEnable(&color, 0, blending);
        dkCmdBufBindColorState(cmd, &color);
        if (blending) {
            DkBlendState b;
            dkBlendStateDefaults(&b);
            dkBlendStateSetOps(&b, blend_op(blend_key >> 11 & 7), DkBlendOp_Add);
            dkBlendStateSetFactors(&b, blend_factor(blend_key >> 1 & 31, dual), blend_factor(blend_key >> 6 & 31, dual),
                                   DkBlendFactor_One, DkBlendFactor_Zero);
            dkCmdBufBindBlendState(cmd, 0, &b);
        }
    }
    if (!n->bound_valid || (n->bound_key >> 14 & 15) != mask) {
        DkColorWriteState writes;
        dkColorWriteStateDefaults(&writes);
        dkColorWriteStateSetMask(&writes, 0, mask);
        dkCmdBufBindColorWriteState(cmd, &writes);
    }
    if (!n->bound_valid || (n->bound_key >> 18 & 31) != depth) {
        DkDepthStencilState d;
        dkDepthStencilStateDefaults(&d);
        d.depthTestEnable = depth & 16 ? 1 : 0;
        d.depthWriteEnable = depth & 16 && depth & 8 ? 1 : 0;
        d.depthCompareOp = depth_compare(depth & 7);
        dkCmdBufBindDepthStencilState(cmd, &d);
    }
    n->bound_key = key;
    n->bound_valid = 1;
}

static const Surface *surface_of(const DhdrPicture *p, DhdrNative *n, int index, const DhdrSurfaceInfo **info)
{
    if (index < 0 || index >= SURFACES || (uint32_t)index >= p->surface_count) return NULL;
    *info = &p->surfaces[index];
    if (!(*info)->size[0] && !(*info)->size[1]) return NULL;
    return &n->surfaces[index];
}

/* DHD_NX_DRAWS=N (a test tool, finding 270): picture N's surfaces and commands, each command with
   what became of it, into draws_N.csv beside dhd.log. */
static FILE *draws_trace;
static void trace_command(const DhdrCommand *k, uint32_t i, const char *what)
{
    if (!draws_trace) return;
    const DhdrNativeConstants *c = &k->constants;
    fprintf(draws_trace, "cmd,%u,%u,%d,%d,%d,%d,%d,%d,%d,%#x,%u,%u,%d,%d,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%u,%u,%#x,%#x,%u,%d,%d,%d,%d,%d,%d,%s\n",
            i, k->kind, k->target, k->depth, k->texture_surface, k->convert, k->texture != NULL, k->replacement != NULL,
            k->source, c->flags, c->tex_size[0], c->tex_size[1], c->tex_offset[0], c->tex_offset[1], c->wrap[0], c->wrap[1],
            c->region[0], c->region[1], c->region[2], c->region[3], k->scissor[0], k->scissor[1], k->scissor[2], k->scissor[3],
            k->first, k->count, k->blend_key, k->color_mask, k->depth_key, k->x, k->y, k->w, k->h, k->sx, k->sy, what);
}

static void trace_begin(const DhdrPicture *p)
{
    static int wanted = -2;
    if (wanted == -2) {
        const char *v = getenv("DHD_NX_DRAWS");
        wanted = v && *v ? atoi(v) : -1;
    }
    if (wanted < 0 || nx_pictures != (uint64_t)wanted) return;
    char path[128];
    snprintf(path, sizeof path, "sdmc:/switch/downhill-revived/draws_%d.csv", wanted);
    if (!(draws_trace = fopen(path, "w"))) return;
    fprintf(draws_trace, "picture,%llu,commands,%u,converts,%u,surfaces,%u,display_surface,%d,display,%u,%u,%u,%u,%u,%u,%u,%u,scale,%u\n",
            (unsigned long long)nx_pictures, p->command_count, p->convert_count, p->surface_count, p->display_surface,
            p->display[0], p->display[1], p->display[2], p->display[3], p->display[4], p->display[5], p->display[6], p->display[7], p->scale);
    fputs("surface,index,page,bw,wide,depth,zpsm,scale,w,h,extent_w,extent_h\n", draws_trace);
    for (uint32_t s = 0; s < p->surface_count; s++) {
        const DhdrSurfaceInfo *f = &p->surfaces[s];
        fprintf(draws_trace, "surface,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", s, f->page, f->bw, f->wide, f->depth, f->zpsm,
                f->scale, f->size[0], f->size[1], f->extent[0], f->extent[1]);
    }
    fputs("cmd,i,kind,target,depth,texture_surface,convert,texture,replacement,source,flags,tex_w,tex_h,tex_x,tex_y,wrap_u,wrap_v,"
          "region0,region1,region2,region3,sc_l,sc_t,sc_r,sc_b,first,count,blend_key,color_mask,depth_key,x,y,w,h,sx,sy,what\n", draws_trace);
}

static void trace_end(void)
{
    if (!draws_trace) return;
    fclose(draws_trace);
    draws_trace = NULL;
    nx_logf("nx draws: picture %llu traced", (unsigned long long)nx_pictures);
}

static void render(DhdrNative *n, const DhdrPicture *p)
{
    int last_convert = -1;
    const Image *converted = NULL;
    uint64_t versions[SURFACES] = {0};
    uint64_t stamps[CONVERTS][4];
    const Image *conversions[CONVERTS] = {0};
    trace_begin(p);
    for (uint32_t i = 0; i < p->command_count; i++) {
        const DhdrCommand *k = &p->commands[i];
#define SKIP(why) { trace_command(k, i, why); continue; }
        if (k->kind == DHDR_CMD_ALPHA_UPLOAD || k->kind == DHDR_CMD_IMPORT || k->kind == DHDR_CMD_COPY) {
            last_convert = -1;
            const DhdrSurfaceInfo *info;
            if (k->kind == DHDR_CMD_IMPORT && (!surface_of(p, n, k->target, &info) || info->depth)) SKIP("skip: import target");
            if (k->kind == DHDR_CMD_COPY && (!surface_of(p, n, k->source, &info) || !surface_of(p, n, k->target, &info))) SKIP("skip: copy surfaces");
            if (k->target >= 0 && k->target < SURFACES) versions[k->target]++;
            pass_end(n);
            if (k->kind == DHDR_CMD_ALPHA_UPLOAD) surface_alpha_upload(n, k);
            else if (k->kind == DHDR_CMD_IMPORT) surface_import(n, k);
            else surface_copy(n, k);
            SKIP("done");
        }
        if (!k->count) SKIP("skip: no vertices");
        const DhdrSurfaceInfo *target_info = NULL, *depth_info = NULL;
        const Surface *target = surface_of(p, n, k->target, &target_info);
        if (!target || target_info->depth) SKIP("skip: target");
        const Surface *depth = k->depth >= 0 && k->depth_key & 8 ? surface_of(p, n, k->depth, &depth_info) : NULL;
        if (depth && (!depth_info->depth || depth_info->size[0] != target_info->size[0] || depth_info->size[1] != target_info->size[1])) depth = NULL;

        /* At a pass boundary, the conversions of the draws to come in this pass (render.rs): each would
           otherwise end the pass it is needed in. Their inputs can't change in the run. */
        if (n->target != k->target || n->depth != (depth ? k->depth : -1)) {
            uint64_t prepared = 0;
            int batched = 0;   /* a conversion of this run recorded: the next needs no barrier */
            for (uint32_t j = i; j < p->command_count; j++) {
                const DhdrCommand *next = &p->commands[j];
                if (next->kind != DHDR_CMD_DRAW || next->target != k->target || next->depth != k->depth ||
                    (next->depth_key & 8) != (k->depth_key & 8)) break;
                int ci = next->convert;
                if (ci < 0 || ci == last_convert || next->texture_surface >= 0 || next->replacement || next->texture ||
                    ci >= CONVERTS || (uint32_t)ci >= p->convert_count || prepared >> ci & 1) continue;
                prepared |= 1ull << ci;
                const DhdrConvert *v = &p->converts[ci];
                uint64_t stamp[4] = {0};
                int in_pass = 0;
                for (int s = 0; s < 4; s++) {
                    if (v->surface[s] < 0) continue;
                    in_pass |= v->surface[s] == k->target || v->surface[s] == k->depth;
                    if (v->surface[s] < SURFACES) stamp[s] = versions[v->surface[s]];
                }
                if (in_pass || (conversions[ci] && !memcmp(stamps[ci], stamp, sizeof stamp))) continue;
                const Image *made = convert_run(n, p, ci, batched);
                if (made) { conversions[ci] = made; memcpy(stamps[ci], stamp, sizeof stamp); batched = convert_batch(); }
            }
        }

        uint32_t tw = target_info->size[0], th = target_info->size[1], big_t = target_info->scale, big_s = 1;
        uint32_t sampler = SAMPLER_NEAREST;
        const Image *source = &dummy;
        int reads_surface = 0;
        if (k->texture_surface >= 0) {
            const DhdrSurfaceInfo *info;
            if (surface_of(p, n, k->texture_surface, &info)) big_s = info->scale;
            else if ((uint32_t)k->texture_surface < p->surface_count) big_s = p->surfaces[k->texture_surface].scale;
            if (k->texture_surface == k->target) {
                pass_end(n);
                if (!sample_target(n, k->texture_surface, &k->constants)) SKIP("skip: sample target");
                source = &n->scratch;
                reads_surface = 1;
            } else if (k->texture_surface < SURFACES && n->surfaces[k->texture_surface].exists && !n->surfaces[k->texture_surface].depth) {
                source = &n->surfaces[k->texture_surface].image;
                reads_surface = 1;
            }
        } else if (k->replacement) {
            const DhdrNativeConstants *c = &k->constants;
            sampler = SAMPLER_REPLACE + ((c->flags >> 5 & 1) | (uint32_t)(c->wrap[0] != 0) << 1 | (uint32_t)(c->wrap[1] != 0) << 2);
            source = &k->replacement->image;
        } else if (k->texture) {
            source = &k->texture->image;
        } else if (k->convert >= 0) {
            if (k->convert != last_convert) {
                int ci = k->convert;
                const DhdrConvert *v = (uint32_t)ci < p->convert_count ? &p->converts[ci] : NULL;
                uint64_t stamp[4] = {0};
                if (v) for (int s = 0; s < 4; s++) stamp[s] = v->surface[s] >= 0 && v->surface[s] < SURFACES ? versions[v->surface[s]] : 0;
                if (ci < CONVERTS && conversions[ci] && v && !memcmp(stamps[ci], stamp, sizeof stamp)) {
                    converted = conversions[ci];
                } else {
                    converted = convert_run(n, p, ci, 0);
                    if (ci < CONVERTS && converted) { conversions[ci] = converted; memcpy(stamps[ci], stamp, sizeof stamp); }
                }
                last_convert = ci;
            }
            if (!converted) SKIP("skip: conversion");
            source = converted;
            reads_surface = 1;
        }
        if (k->convert < 0) last_convert = -1;
        if (reads_surface) {
            /* its pixels must be out of the 3D engine first, even those of the pass still open */
            if (n->target != k->target) pass_end(n);
            if (n->writes) surfaces_sync(n);
        }

        int retained = !depth && n->target == k->target && n->depth >= 0;
        int depth_index = depth ? k->depth : retained ? n->depth : -1;
        if (n->target != k->target || n->depth != depth_index) {
            pass_end(n);
            if (n->sampled) surfaces_sync(n);   /* it may write what a draw is still reading */
            /* finding 270: ZCULL keeps coarse Z per tile for the bound Z image and is dropped only when
               the bound address changes; a Z surface written or read elsewhere since (another pass,
               a copy, a probe) left it stale, and stale tiles reject whole blocks of a scene */
            flush_draw(n);
            dkCmdBufBindRenderTarget(cmd, &n->surfaces[k->target].image.view, depth ? &n->surfaces[k->depth].image.view : NULL);
            if (depth || n->zcull_stale) {
                barrier(DkBarrier_None, DkInvalidateFlags_Zcull);
                n->zcull_stale = 0;
            }
            n->target = k->target, n->depth = depth_index;
            n->viewport_valid = n->scissor_valid = 0;
        }
        if (reads_surface) n->sampled = 1;
        DhdrNativeConstants constants = k->constants;
        constants.scale = big_t | big_s << 8 | (big_s < big_t ? big_s : big_t) << 16;
        constants.target[0] = (float)tw / (float)big_t, constants.target[1] = (float)th / (float)big_t;
        int t = (int)big_t;
        int left = k->scissor[0] * t, top = k->scissor[1] * t, right = k->scissor[2] * t, bottom = k->scissor[3] * t;
        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right > (int)tw) right = (int)tw;
        if (bottom > (int)th) bottom = (int)th;
        if (right <= left || bottom <= top) SKIP("skip: scissor");

        uint32_t blend_key = k->blend_key, mask = k->color_mask, dk = k->depth_key;
        uint32_t src = blend_key >> 1 & 31, dst = blend_key >> 6 & 31;
        int enabled = blend_key & 1;
        int uses_as = enabled && (src == BLEND_SA || src == BLEND_ISA || dst == BLEND_SA || dst == BLEND_ISA);
        int uses_constant = enabled && (src == BLEND_FACTOR || src == BLEND_INV_FACTOR || dst == BLEND_FACTOR || dst == BLEND_INV_FACTOR);
        uint32_t depth_bits = depth ? 16 | (dk & 4 ? 8u : 0u) | (dk & 3) : retained ? 17 : 0;
        uint32_t key = blend_key | mask << 14 | depth_bits << 18 | (uint32_t)uses_as << 23;
        if (k->constants.flags & 4096) key |= KEY_MESH | (k->constants.flags & 8192 ? KEY_TWO_SIDED : 0);

        DkGpuAddr uniform = n->constants_addr;
        if (!n->constants_valid || memcmp(&n->constants_last, &constants, sizeof constants)) {
            uniform = constants_of(&constants, sizeof constants);
            if (uniform == DK_GPU_ADDR_INVALID) SKIP("skip: uniform arena full");
            n->constants_last = constants, n->constants_addr = uniform, n->constants_valid = 1;
        }
        uint32_t handle = dkMakeTextureHandle((uint32_t)source->sampled, sampler);
        uint32_t scissor[4] = {(uint32_t)left, (uint32_t)top, (uint32_t)(right - left), (uint32_t)(bottom - top)};
        int same = n->pending_count && n->bound_valid && n->bound_key == key && n->uniform_set && n->uniform_bound == uniform &&
                   n->texture_bound && (uint32_t)n->blend_set == (uses_constant ? k->blend_fix : n->blend_set) &&
                   (uses_constant ? n->blend_valid != 0 : 1) && n->scissor_valid && !memcmp(n->scissor, scissor, sizeof scissor) &&
                   n->viewport_valid && n->viewport[0] == tw && n->viewport[1] == th && (uint32_t)n->texture_bound == handle + 1;
        if (!same) {
            flush_draw(n);
            set_key(n, key);
            if (!n->viewport_valid || n->viewport[0] != tw || n->viewport[1] != th) {
                DkViewport viewport = {0.0f, 0.0f, (float)tw, (float)th, 0.0f, 1.0f};
                dkCmdBufSetViewports(cmd, 0, &viewport, 1);
                n->viewport[0] = tw, n->viewport[1] = th, n->viewport_valid = 1;
            }
            if (!n->scissor_valid || memcmp(n->scissor, scissor, sizeof scissor)) {
                DkScissor s = {scissor[0], scissor[1], scissor[2], scissor[3]};
                dkCmdBufSetScissors(cmd, 0, &s, 1);
                memcpy(n->scissor, scissor, sizeof scissor);
                n->scissor_valid = 1;
            }
            if (uses_constant && (!n->blend_valid || n->blend_set != k->blend_fix)) {
                float fix = (float)k->blend_fix / 128.0f;
                dkCmdBufSetBlendConst(cmd, fix, fix, fix, fix);
                n->blend_set = k->blend_fix, n->blend_valid = 1;
            }
            if (!n->uniform_set || n->uniform_bound != uniform) {
                dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, uniform, (uint32_t)sizeof constants);
                dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, uniform, (uint32_t)sizeof constants);
                n->uniform_bound = uniform, n->uniform_set = 1;
            }
            if ((uint32_t)n->texture_bound != handle + 1) {
                dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, handle);
                n->texture_bound = (int)(handle + 1);
            }
        }
        if (n->pending_count && n->pending_count % 3 == 0 && n->pending_first + n->pending_count == (int32_t)k->first) {
            n->pending_count += (int32_t)k->count;
        } else {
            flush_draw(n);
            n->pending_first = (int32_t)k->first, n->pending_count = (int32_t)k->count;
        }
        n->drawn.draws++;
        if (k->target >= 0 && k->target < SURFACES) versions[k->target]++;
        if (depth && k->depth < SURFACES) versions[k->depth]++;
        trace_command(k, i, source == &dummy && k->constants.flags & 1 ? "drawn: textured, no source" : "drawn");
#undef SKIP
    }
    pass_end(n);
    trace_end();
}

/* DHD_DIRECT_PRESENT (on unless 0, as main's; finding 247): a picture that lies as one rectangle in its
   display surface, rows doubled or not, is not copied into the display image; the present samples it
   from the surface (native.wgsl's present_direct, the same sampler on the same texels: the copy was a
   plain one, nearest, every row twice when doubled). The display image keeps its size and is filled on
   demand (materialize). Off: the scanout's copy every picture, as before. */
static int direct_present(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("DHD_DIRECT_PRESENT");
        on = !(v && !strcmp(v, "0"));
        nx_logf("nx present: %s (DHD_DIRECT_PRESENT)",
                on ? "from the display surface when it holds the picture as one rectangle" : "from the display image, a copy every picture");
    }
    return on;
}

/* The scanout's copy: rows (sx, sy, w, source_h) of a surface into the display image's top left, w x h
   (each row twice when h is twice source_h), by the 2D engine. */
static void scanout_blit(DhdrNative *n, const Image *from, uint32_t sx, uint32_t sy, uint32_t w, uint32_t source_h, uint32_t h)
{
    DkImageRect a, b;
    copy_rect(&a, sx, sy, w, source_h);
    copy_rect(&b, 0, 0, w, h);
    surfaces_sync(n);
    dkCmdBufBlitImage(cmd, &from->view, &a, &n->display.view, &b, DkBlitFlag_FilterNearest, 0);
    n->writes = n->sampled = 1;
}

/* The copy a picture shown from its surface skipped, now (scanout.rs's materialize): for what reads the
   display image itself (the test captures) and before a picture that shows nothing new draws into
   the surface. From here on the present samples the display image. */
static void materialize(DhdrNative *n)
{
    int i = n->direct_surface;
    n->direct_surface = -1;
    if (i < 0 || i >= SURFACES || !n->has_display) return;
    const Surface *s = &n->surfaces[i];
    const uint32_t *r = n->direct_rect;
    if (!s->exists || s->depth || r[0] + r[2] > s->size[0] || r[1] + r[3] > s->size[1]) return;
    scanout_blit(n, &s->image, r[0], r[1], r[2], r[3], n->direct_h);
}

/* Whether picture p shows something of its own (a movie, its display surface), so a picture held on its
   surface only can be left there: the tests of picture()'s scanout. */
static int will_show(const DhdrNative *n, const DhdrPicture *p)
{
    if (p->movie_on && p->movie) return 1;
    if (p->display_surface < 0 || (uint32_t)p->display_surface >= p->surface_count || p->display_surface >= SURFACES) return 0;
    const Surface *s = &n->surfaces[p->display_surface];
    return s->exists && p->display[5] && p->display[6] && p->display[3] * s->scale < s->size[0] &&
           p->display[4] * s->scale < s->size[1];
}

static int picture(DhdrNative *n, const DhdrPicture *p, const uint32_t *indices, uint32_t index_count, DhdrPictureResult *out)
{
    frame_begin();
    memset(&n->drawn, 0, sizeof n->drawn);
    n->indexed = indices != NULL;
    n->constants_valid = n->uniform_set = n->texture_bound = 0;
    n->bound_valid = n->blend_valid = n->scissor_valid = n->viewport_valid = 0;
    n->rect_pixel = NULL;
    n->pending_count = 0;
    uint64_t record = now_ns();
    /* The GPU keeps these for good: the descriptor tables and the vertex buffer's stride never change;
       the rasterizer and the vertex layout change only with a mesh key (set_key, raster_bind). */
    static int bound_once;
    if (!bound_once) {
        bound_once = 1;
        dkCmdBufBindImageDescriptorSet(cmd, image_set, IMAGE_DESCRIPTORS);
        dkCmdBufBindSamplerDescriptorSet(cmd, sampler_set, SAMPLER_DESCRIPTORS);
        raster_culls = -1;
        raster_bind(0);
        dkCmdBufBindVtxAttribState(cmd, vertex_attributes, 3);
        dkCmdBufBindVtxBufferState(cmd, &vertex_buffer, 1);
    }
    /* First, so running out here loses no texture: uploads_pass gives up its pixels. */
    void *cpu;
    uint32_t vertex_bytes = p->vertex_count * sizeof(DhdrVertex);
    DkGpuAddr vertices = arena(vertex_bytes ? vertex_bytes : 32, 32, &cpu);
    if (vertices == DK_GPU_ADDR_INVALID) return 0;
    if (vertex_bytes) memcpy(cpu, p->vertices, vertex_bytes);
    dkCmdBufBindVtxBuffer(cmd, 0, vertices, vertex_bytes ? vertex_bytes : 32);
    if (indices) {
        DkGpuAddr index = arena(index_count * 4, 4, &cpu);
        if (index == DK_GPU_ADDR_INVALID) return 0;
        memcpy(cpu, indices, index_count * 4);
        dkCmdBufBindIdxBuffer(cmd, DkIdxFormat_Uint32, index);
    }
    uploads_pass();
    surfaces_prepare(n);
    /* the picture held on its surface (finding 247) is copied out before this one draws there, unless
       this one replaces it; a grown surface holds its old contents again by here */
    if (n->direct_surface >= 0 && !will_show(n, p)) materialize(n);
    n->direct_surface = -1;
    render(n, p);

    uint32_t shown = 0;
    if (p->movie_on && p->movie) {
        uint32_t w = p->movie_width, h = p->movie_height;
        if (!n->has_display || n->display_w != w || n->display_h != h) {
            if (n->has_display) image_drop(&n->display);
            n->has_display = image_make(&n->display, w, h, DkImageFormat_RGBA8_Unorm, DkImageFlags_Usage2DEngine, 1);
            n->display_w = n->has_display ? w : 0, n->display_h = n->has_display ? h : 0;
        }
        if (n->has_display) {
            surfaces_sync(n);
            upload(&n->display, p->movie, w, h, 0, 0, w, h);
            copied(n);
            shown = 1;
        }
    } else if (p->display_surface >= 0 && (uint32_t)p->display_surface < p->surface_count && p->display_surface < SURFACES &&
               n->surfaces[p->display_surface].exists) {
        const Surface *s = &n->surfaces[p->display_surface];
        uint32_t d[12];
        memcpy(d, p->display, sizeof d);
        d[5] *= s->scale, d[6] *= s->scale, d[8] = s->scale;
        if (d[5] && d[6]) {
            if (!n->has_display || n->display_w != d[5] || n->display_h != d[6]) {
                if (n->has_display) image_drop(&n->display);
                n->has_display = image_make(&n->display, d[5], d[6], DkImageFormat_RGBA8_Unorm, DkImageFlags_Usage2DEngine, 1);
                n->display_w = n->has_display ? d[5] : 0, n->display_h = n->has_display ? d[6] : 0;
            }
            /* native_scanout's sums, by the 2D engine: rows doubled when d[7] shifts them, the origin
               scaled. uam's compute output came out in broken blocks (its unpatched dual issue);
               a blit needs no shader and is cheaper. */
            if (n->has_display) {
                uint32_t shift = d[7] & 31, sx = d[3] * s->scale, sy = d[4] * s->scale;
                uint32_t w = d[5], h = d[6], source_h = (h + (1u << shift) - 1) >> shift;
                if (sx < s->size[0] && sy < s->size[1]) {
                    if (w > s->size[0] - sx) w = s->size[0] - sx;
                    if (source_h > s->size[1] - sy) source_h = s->size[1] - sy;
                    if (h > source_h << shift) h = source_h << shift;
                    /* finding 270: a surface smaller than the picture leaves the display's other
                       pixels as the last picture had them (native.wgsl reads them black) */
                    static uint32_t told_w, told_h, told_surface = ~0u;
                    if ((w < d[5] || h < d[6]) && (told_w != w || told_h != h || told_surface != (uint32_t)p->display_surface)) {
                        told_w = w, told_h = h, told_surface = (uint32_t)p->display_surface;
                        nx_logf("nx scanout: surface %d (%u x %u, bw %u, extent %u x %u) gives %u x %u of %u x %u from (%u, %u)",
                                p->display_surface, s->size[0], s->size[1], p->surfaces[p->display_surface].bw,
                                p->surfaces[p->display_surface].extent[0], p->surfaces[p->display_surface].extent[1],
                                w, h, d[5], d[6], sx, sy);
                    }
                    /* one rectangle, nothing cut off, each row once or exactly twice: the present
                       samples it where it lies (finding 247); else the copy */
                    if (direct_present() && shift <= 1 && w == d[5] && h == d[6] && !(shift && h & 1)) {
                        n->direct_surface = p->display_surface;
                        n->direct_rect[0] = sx, n->direct_rect[1] = sy, n->direct_rect[2] = w, n->direct_rect[3] = source_h;
                        n->direct_h = h;
                    } else {
                        scanout_blit(n, &s->image, sx, sy, w, source_h, h);
                    }
                    shown = 1;
                }
            }
        }
    }
    nx_render_stage = "submitting";
    nx_t_record_ns += now_ns() - record;
    submit();
    nx_render_stage = "picture done";
    nx_pictures++, nx_draws = n->drawn.draws, nx_shown = shown, nx_commands = p->command_count;
    *out = n->drawn;
    out->shown = shown;
    out->display_width = n->display_w;
    out->display_height = n->display_h;
    return 1;
}
int dhdr_native_picture(DhdrNative *n, const DhdrPicture *p, DhdrPictureResult *out) { return picture(n, p, NULL, 0, out); }
int dhdr_native_picture_indexed(DhdrNative *n, const DhdrPicture *p, const uint32_t *indices, uint32_t index_count, DhdrPictureResult *out)
{
    return picture(n, p, indices, index_count, out);
}

/* ---- the screen ---- */
struct DhdrSurface {
    DkSwapchain swapchain;
    Image images[SWAP_IMAGES];
    DkImage const *list[SWAP_IMAGES];
    DkMemBlock block;
    int acquired, w, h;
    int vsync, interval;  /* dhd.ini's vsync; the swap interval set last (-1: none yet) */
};

DhdrSurface *dhdr_surface_create(const DhdrWindow *w, uint32_t present_flags)
{
    if (!ready || w->kind != DHDR_WINDOW_NX) return NULL;
    DhdrSurface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    nx_screen_size(&s->w, &s->h);
    DkImageLayoutMaker maker;
    DkImageLayout layout;
    dkImageLayoutMakerDefaults(&maker, device);
    maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    maker.format = DkImageFormat_RGBA8_Unorm;
    maker.dimensions[0] = (uint32_t)s->w, maker.dimensions[1] = (uint32_t)s->h;
    dkImageLayoutInitialize(&layout, &maker);
    uint32_t align = dkImageLayoutGetAlignment(&layout), size = (uint32_t)dkImageLayoutGetSize(&layout);
    size = (size + align - 1) & ~(align - 1);
    s->block = block_make(SWAP_IMAGES * size, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    for (int i = 0; i < SWAP_IMAGES; i++) {
        dkImageInitialize(&s->images[i].image, &layout, s->block, (uint32_t)i * size);
        dkImageViewDefaults(&s->images[i].view, &s->images[i].image);
        s->list[i] = &s->images[i].image;
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, device, (void *)(uintptr_t)w->a, s->list, SWAP_IMAGES);
    s->swapchain = dkSwapchainCreate(&sm);
    s->acquired = -1;
    s->vsync = (present_flags & DHDR_PRESENT_NO_TEARING) != 0;
    s->interval = -1;
    screen = s;
    return s;
}
void dhdr_surface_destroy(DhdrSurface *s)
{
    if (!s) return;
    dkQueueWaitIdle(queue);
    dkSwapchainDestroy(s->swapchain);
    dkMemBlockDestroy(s->block);
    if (screen == s) screen = NULL;
    free(s);
}

/* EXIT GAME's way back to the Homebrew Menu (nx_host.c), after the render thread has stopped: every
   block goes back to the heap as it was (its attributes and GPU mapping undone), which the loader
   needs before it reloads the menu (finding 270). */
void nx_render_release(void)
{
    if (ready <= 0) return;
    dkQueueWaitIdle(queue);
    dhdr_surface_destroy(screen);
    dkCmdBufDestroy(cmd);
    dkQueueDestroy(queue);
    for (int i = 0; i < FRAMES; i++) {
        dkMemBlockDestroy(frames[i].arena);
        dkMemBlockDestroy(frames[i].commands);
    }
    DkMemBlock blocks[] = {code_block, heap_block, descriptor_block, zread_block, capture_block};
    for (size_t i = 0; i < sizeof blocks / sizeof *blocks; i++)
        if (blocks[i]) dkMemBlockDestroy(blocks[i]);
    dkDeviceDestroy(device);
    ready = 0;
}
uint32_t dhdr_surface_usage(const DhdrSurface *s) { (void)s; return 0; }
void dhdr_surface_counter(DhdrSurface *s, int enabled, int server_ms, int race_ms, int riders_ms, double seconds)
{
    (void)s; (void)enabled; (void)server_ms; (void)race_ms; (void)riders_ms; (void)seconds;
}
/* The touch overlay (finding 141) is not drawn on the Switch yet. */
void dhdr_surface_touch(DhdrSurface *s, const float *items, uint32_t count, float width_mm, float height_mm)
{
    (void)s; (void)items; (void)count; (void)width_mm; (void)height_mm;
}
/* The swap interval follows the time a picture is held (fps.c port_frame_period), in whole
   vblanks of the 60 Hz screen: at fps = 30 every picture stays two vblanks, so the display paces
   the frames evenly instead of the wall-clock grid against vblank (pictures held one, two or three
   vblanks). 1/60 before GO and at 60 (1 vblank); a rate between 30 and 60 keeps 1. vsync = 0: 0. */
extern double port_frame_period(void);
static void present_interval(DhdrSurface *s)
{
    int interval = 0;
    if (s->vsync) {
        interval = (int)(port_frame_period() * 60 + 0.01);
        if (interval < 1) interval = 1;
        if (interval > 2) interval = 2;
    }
    if (interval == s->interval) return;
    dkSwapchainSetSwapInterval(s->swapchain, (uint32_t)interval);
    nx_logf("nx present: swap interval %d", interval);
    s->interval = interval;
}
void dhdr_surface_present(DhdrSurface *s)
{
    if (s->acquired < 0) return;
    uint64_t start = now_ns();
    queue_check("a present");
    present_interval(s);
    dkQueuePresentImage(queue, s->swapchain, s->acquired);
    s->acquired = -1;
    nx_t_present_ns += now_ns() - start;
}

/* DHD_NX_FPS=1 in dhd.ini: game fields per second, top left of the screen, updated once a second.
   The game's own speed, read here because a deko3d homebrew has none of the NVN, EGL or Vulkan
   present calls that overlays count. Scissored clears: no shader, nothing to bind. */
extern uint64_t kernel_vsync_count;

static void fps_rect(DkCmdBuf cmd, int x, int y, int w, int h, float r, float g, float b)
{
    DkScissor box = {(uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h};
    dkCmdBufSetScissors(cmd, 0, &box, 1);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, r, g, b, 1.0f);
}

static void fps_overlay(DkCmdBuf cmd, int screen_h)
{
    /* the segments of 0-9: bit 0 top, 1 upper right, 2 lower right, 3 bottom, 4 lower left,
       5 upper left, 6 middle */
    static const uint8_t digits[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    static int on, shown = -1;
    static uint64_t next, last_at, last_field;
    uint64_t at = now_ns();
    if (at >= next) {
        const char *s = getenv("DHD_NX_FPS");  /* dhd.ini's lines arrive after the start */
        on = s && *s && strcmp(s, "0");
        uint64_t field = *(volatile uint64_t *)&kernel_vsync_count;
        if (last_at) shown = (int)((double)(field - last_field) * 1e9 / (double)(at - last_at) + 0.5);
        last_at = at, last_field = field, next = at + 1000000000u;
    }
    if (!on || shown < 0) return;
    static int told;
    if (!told++) nx_logf("nx: speed counter on, screen height %d", screen_h);
    int value = shown > 999 ? 999 : shown, count = value >= 100 ? 3 : value >= 10 ? 2 : 1;
    int u = screen_h / 90, w = 5 * u, h = 9 * u, gap = 2 * u, pad = 2 * u, x = 4 * u, y = 4 * u;
    fps_rect(cmd, x, y, 2 * pad + count * w + (count - 1) * gap, 2 * pad + h, 0.0f, 0.0f, 0.0f);
    for (int i = count - 1, power = 1; i >= 0; i--, power *= 10) {
        uint8_t on_segments = digits[value / power % 10];
        int left = x + pad + i * (w + gap), top = y + pad, half = (h - u) / 2;
        const struct { int x, y, w, h; } segment[7] = {
            {left, top, w, u}, {left + w - u, top, u, half + u}, {left + w - u, top + half, u, half + u},
            {left, top + h - u, w, u}, {left, top + half, u, half + u}, {left, top, u, half + u},
            {left, top + half, w, u},
        };
        for (int k = 0; k < 7; k++)
            if (on_segments >> k & 1)
                fps_rect(cmd, segment[k].x, segment[k].y, segment[k].w, segment[k].h, 0.2f, 1.0f, 0.2f);
    }
}

/* DHD_NX_CAPTURE=1 (a test tool): images as BMP files beside dhd.log, at a few pictures. */
static void capture_image(const Image *t, const char *name, uint64_t picture)
{
    enum { CAPTURE_SIZE = 32 << 20 };
    uint32_t bytes = t->w * t->h * 4;
    if (bytes > CAPTURE_SIZE) return;
    if (!capture_block) capture_block = block_make(CAPTURE_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    DkCopyBuf to = {dkMemBlockGetGpuAddr(capture_block), t->w * 4, t->h};
    DkImageRect rect;
    copy_rect(&rect, 0, 0, t->w, t->h);
    barrier(DkBarrier_Full, DkInvalidateFlags_Image);
    dkCmdBufCopyImageToBuffer(cmd, &t->view, &rect, &to, 0);
    barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);
    submit();
    dkQueueWaitIdle(queue);
    char path[128];
    snprintf(path, sizeof path, "sdmc:/switch/downhill-revived/cap_%llu_%s.bmp", (unsigned long long)picture, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t header[13] = {0, 0, 54, 40, t->w, t->h, 1u | 32u << 16, 0, bytes, 2835, 2835, 0, 0};
    uint32_t total = 54 + bytes;
    header[0] = total;
    fwrite("BM", 1, 2, f);
    fwrite(header, 4, 13, f);
    const uint8_t *from = dkMemBlockGetCpuAddr(capture_block);
    uint8_t *row = malloc(t->w * 4);
    for (uint32_t y = t->h; y--;) {
        for (uint32_t x = 0; x < t->w; x++) {
            const uint8_t *p = from + (y * t->w + x) * 4;
            row[x * 4] = p[2], row[x * 4 + 1] = p[1], row[x * 4 + 2] = p[0], row[x * 4 + 3] = 255;
        }
        fwrite(row, 4, t->w, f);
    }
    free(row);
    fclose(f);
    nx_logf("nx capture: %s %u x %u at picture %llu", path, t->w, t->h, (unsigned long long)picture);
}

/* A capture writes about ten BMPs to the card, about 4.7 s with the game stopped: none inside DHD_BENCH's
   window, whose speed it would ruin (phase B's menus measured 26.8 fields/s with two of them; finding
   270). One comes right after the window instead. */
static int capture_pass(DhdrNative *n)
{
    static int on = -1, next, after_window;
    static unsigned long long first, last;
    static const uint64_t at[] = {120, 400, 900, 1500, 2200, 5800};
    enum { AT = sizeof at / sizeof *at };
    if (on < 0) {
        on = getenv("DHD_NX_CAPTURE") != NULL;
        const char *window = getenv("DHD_BENCH");
        if (!window || sscanf(window, "%llu-%llu", &first, &last) != 2 || last <= first) first = last = 0;
    }
    if (!on) return 0;
    while (next < AT && last && at[next] >= first && at[next] <= last) next++, after_window = 1;
    if (after_window && nx_pictures > last) after_window = 0;
    else if (next >= AT || nx_pictures < at[next] || (last && nx_pictures >= first && nx_pictures <= last)) return 0;
    else next++;
    /* a picture on its surface only (finding 247): copied into the display image for its capture, and
       still presented from the surface, so the screen capture shows present_direct's picture */
    if (n->direct_surface >= 0) {
        int held = n->direct_surface;
        materialize(n);
        n->direct_surface = held;
    }
    capture_image(&n->display, "display", nx_pictures);
    nx_logf("nx capture: display %u x %u, heap %u MiB", n->display_w, n->display_h, heap_used >> 20);
    for (int i = 0; i < SURFACES; i++)
        if (n->surfaces[i].exists && !n->surfaces[i].depth) {
            char name[16];
            snprintf(name, sizeof name, "s%d", i);
            capture_image(&n->surfaces[i].image, name, nx_pictures);
        }
    return 1;
}

/* The present's drawing into target (w x h): black, then the picture in r's rectangle, cut to r's cut,
   from the display image or, for a picture on its surface only, from that surface (present_direct). */
static void present_draw(DhdrNative *n, const DkImageView *target, int w, int h, const DhdrPresentRect *r)
{
    /* native.wgsl's Present: rect; cover and source (the widescreen fill's, not drawn here); then
       present_direct's map (the picture's size, its origin in the surface) and mode (rows doubled,
       0, one over the surface's size) */
    const Surface *direct = n->direct_surface >= 0 ? &n->surfaces[n->direct_surface] : NULL;
    float present[20] = {r->rect[0], r->rect[1], r->rect[2], r->rect[3]};
    if (direct) {
        present[12] = (float)n->direct_rect[2], present[13] = (float)n->direct_h;
        present[14] = (float)n->direct_rect[0], present[15] = (float)n->direct_rect[1];
        present[16] = n->direct_h != n->direct_rect[3] ? 1.0f : 0.0f;
        present[18] = 1.0f / (float)direct->image.w, present[19] = 1.0f / (float)direct->image.h;
    }
    uint32_t present_bytes = direct ? (uint32_t)sizeof present : 16;
    DkGpuAddr uniform = constants_of(present, present_bytes);
    dkCmdBufBindRenderTarget(cmd, target, NULL);
    DkViewport viewport = {0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    dkCmdBufSetViewports(cmd, 0, &viewport, 1);
    /* the whole screen before the clear: clears respect the scissor */
    DkScissor whole = {0, 0, (uint32_t)w, (uint32_t)h};
    dkCmdBufSetScissors(cmd, 0, &whole, 1);
    DkColorState color;
    dkColorStateDefaults(&color);
    dkCmdBufBindColorState(cmd, &color);
    DkColorWriteState writes;
    dkColorWriteStateDefaults(&writes);
    dkCmdBufBindColorWriteState(cmd, &writes);
    DkDepthStencilState depth;
    dkDepthStencilStateDefaults(&depth);
    depth.depthTestEnable = 0, depth.depthWriteEnable = 0;
    dkCmdBufBindDepthStencilState(cmd, &depth);
    n->rect_pixel = NULL;
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    int left = r->cut[0] < 0 ? 0 : r->cut[0], top = r->cut[1] < 0 ? 0 : r->cut[1];
    int right = r->cut[2] > w ? w : r->cut[2], bottom = r->cut[3] > h ? h : r->cut[3];
    if (right > left && bottom > top && uniform != DK_GPU_ADDR_INVALID) {
        DkScissor cut = {(uint32_t)left, (uint32_t)top, (uint32_t)(right - left), (uint32_t)(bottom - top)};
        dkCmdBufSetScissors(cmd, 0, &cut, 1);
        DkShader const *shaders[2] = {&shader_fullscreen, direct ? &shader_present_direct : &shader_present};
        dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
        raster_bind(0);
        dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, uniform, present_bytes);
        const Image *source = direct ? &direct->image : &n->display;
        dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle((uint32_t)source->sampled, n->present_sampler));
        descriptors_sync();
        dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
        n->sampled = 1;
    }
}

int dhdr_native_present(DhdrNative *n, DhdrSurface *s, const DhdrPresentRect *r)
{
    if (!n->has_display || !r->window_w || !r->window_h) return 0;
    if (capture_pass(n)) {
        /* what the present draws, into an image of the screen's size, as BMP: the picture on the
           screen (present_direct's or present_pixel's) without the screen image's compression */
        static Image shot;
        static int has_shot;
        if (!has_shot) has_shot = image_make_with(&shot, (uint32_t)s->w, (uint32_t)s->h, DkImageFormat_RGBA8_Unorm, SURFACE_FLAGS, 1, 0);
        if (has_shot) {
            surfaces_sync(n);
            present_draw(n, &shot.view, s->w, s->h, r);
            capture_image(&shot, "screen", nx_pictures);
        }
    }
    {
        static int told;
        if (told < 3 && told++ >= 0)
            nx_logf("nx present: rect %.1f %.1f %.1f %.1f cut %d %d %d %d window %u x %u display %u x %u", r->rect[0], r->rect[1], r->rect[2], r->rect[3],
                    r->cut[0], r->cut[1], r->cut[2], r->cut[3], r->window_w, r->window_h, n->display_w, n->display_h);
    }
    nx_render_stage = "acquiring a screen image";
    queue_check("acquiring a screen image");
    uint64_t start = now_ns();
    s->acquired = dkQueueAcquireImage(queue, s->swapchain);
    uint64_t record = now_ns();
    nx_t_acquire_ns += record - start;
    nx_render_stage = "presenting";
    nx_presents++;
    surfaces_sync(n);
    present_draw(n, &s->images[s->acquired].view, s->w, s->h, r);
    fps_overlay(cmd, s->h);
    barrier(DkBarrier_Fragments, 0);
    nx_t_present_ns += now_ns() - record;
    submit();
    return 1;
}

/* ---- the Z probe (zread.rs): compute into CPU-visible memory, read when its fence has passed ---- */
int dhdr_native_zread_start(DhdrNative *n, uint32_t index, const uint32_t rect[4], uint64_t tag)
{
    if (index >= SURFACES) return 0;
    Surface *s = &n->surfaces[index];
    if (!s->exists || !s->depth) return 0;
    uint32_t scale = s->scale ? s->scale : 1, cols = s->size[0] / scale, rows = s->size[1] / scale;
    uint32_t x = rect[0], y = rect[1], w = rect[2], h = rect[3];
    if (x >= cols || y >= rows) return 0;
    if (w > ZREAD_MAX) w = ZREAD_MAX;
    if (h > ZREAD_MAX) h = ZREAD_MAX;
    if (w > cols - x) w = cols - x;
    if (h > rows - y) h = rows - y;
    if (!w || !h) return 0;
    int slot = -1;
    for (int i = 0; i < ZREAD_SLOTS; i++) if (!n->zread[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    uint64_t record = now_ns();
    uint32_t params[8] = {x, y, w, h, scale, 0, 0, 0};
    DkGpuAddr uniform = constants_of(params, sizeof params);
    if (uniform == DK_GPU_ADDR_INVALID) return 0;
    surfaces_sync(n);
    DkShader const *shader = &shader_zread;
    dkCmdBufBindShaders(cmd, DkStageFlag_Compute, &shader, 1);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Compute, 0, uniform, sizeof params);
    dkCmdBufBindStorageBuffer(cmd, DkStage_Compute, 0, dkMemBlockGetGpuAddr(zread_block) + (DkGpuAddr)slot * ZREAD_BYTES, ZREAD_BYTES);
    dkCmdBufBindTexture(cmd, DkStage_Compute, 0, dkMakeTextureHandle((uint32_t)s->image.sampled, SAMPLER_NEAREST));
    descriptors_sync();
    dkCmdBufDispatchCompute(cmd, (w + 7) / 8, (h + 7) / 8, 1);
    barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);   /* compute: the one that waits for it */
    nx_t_record_ns += now_ns() - record;
    submit();
    dkQueueSignalFence(queue, &n->zread[slot].fence, true);
    dkQueueFlush(queue);
    n->zread[slot].rect[0] = x, n->zread[slot].rect[1] = y, n->zread[slot].rect[2] = w, n->zread[slot].rect[3] = h;
    n->zread[slot].tag = tag;
    n->zread[slot].used = 1;
    return 1;
}
int dhdr_native_zread_take(DhdrNative *n, uint32_t *out, uint32_t cap, uint32_t rect_out[4], uint64_t *tag_out)
{
    int newest = -1;
    for (int i = 0; i < ZREAD_SLOTS; i++)
        if (n->zread[i].used && dkFenceWait(&n->zread[i].fence, 0) == DkResult_Success) {
            n->zread[i].done = 1;
            if (newest < 0 || n->zread[i].tag > n->zread[newest].tag) newest = i;
        }
    if (newest < 0) return 0;
    uint32_t count = n->zread[newest].rect[2] * n->zread[newest].rect[3];
    int taken = 0;
    if (count <= cap) {
        /* its fence has passed: drop what the CPU may hold of these lines (nothing here is ever dirty) */
        dkMemBlockFlushCpuCache(zread_block, (uint32_t)newest * ZREAD_BYTES, count * 4);
        memcpy(out, (uint8_t *)dkMemBlockGetCpuAddr(zread_block) + newest * ZREAD_BYTES, count * 4);
        memcpy(rect_out, n->zread[newest].rect, 16);
        *tag_out = n->zread[newest].tag;
        taken = 1;
    }
    uint64_t tag = n->zread[newest].tag;
    for (int i = 0; i < ZREAD_SLOTS; i++)
        if (n->zread[i].used && n->zread[i].done && n->zread[i].tag <= tag) n->zread[i].used = n->zread[i].done = 0;
    return taken;
}

/* The tools' capture of the displayed picture: not on the Switch. */
int dhdr_native_read_begin(DhdrNative *n, uint32_t *w, uint32_t *h) { (void)n; *w = *h = 0; return 0; }
int dhdr_native_read_finish(DhdrNative *n, uint32_t *pixels) { (void)n; (void)pixels; return 0; }
