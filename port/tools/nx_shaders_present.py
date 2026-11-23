"""The Switch's present_direct stage (finding 247: the present samples the picture from its display
surface) as DKSH: nx_shaders.py's own build of main's native.wgsl entry point, with the same uam
workarounds, written to a file of its own (build_host64.py runs both).

Kept apart from nx_shaders.py only while another lane changes that file (finding 270); then it
becomes one more line of nx_shaders.py's ENTRIES and this file goes."""
import sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nx_shaders  # noqa: E402


def main():
    out = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as t:
        work = Path(t)
        source = work / 'native.wgsl'
        source.write_text(nx_shaders.wgsl())
        data, pairs, depth = nx_shaders.build(work, source, 'present_direct', 'frag')
        print('present_direct.frag: %d bytes, %d dual-issue pairs undone, branches nested %d deep' % (len(data), pairs, depth))
        out.write_text('/* port/tools/nx_shaders_present.py: DKSH of port/gs/native.wgsl\'s present_direct. */\n'
                       'static const unsigned char dksh_present_direct[] __attribute__((aligned(256))) = {%s};\n'
                       % ','.join(map(str, data)))


if __name__ == '__main__':
    main()
