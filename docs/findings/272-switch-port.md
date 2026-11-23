# 272: The Nintendo Switch port (homebrew NRO)

Status: in progress

The game runs as a homebrew NRO on a modded Switch: title, menus and a race draw on the console.
The target is `build_host64.py --target switch-arm64`, with the same wasm2c core and the same
shared host as every other platform. What stands on an OS or on Rust elsewhere is in `port/nx`.
Speed so far: about 60 fields/s in menus, about 33 on the title and in a race, so not yet 60 in
the race. No release includes this build. Build steps: `docs/BUILDING.md`, "Nintendo Switch".

## The target

| Part | Other platforms | Switch |
|---|---|---|
| Compiler | clang | devkitA64's GCC (`-frounding-math`, `-ffp-contract=off`, `-fno-fast-math`; finding 081's float rules without clang's `-ffp-model`) |
| Window, threads, files, audio | SDL3 | `port/nx/src/sdl_nx.c`, the SDL3 calls the port makes, on libnx (headers in `port/nx/include/SDL3`). The sdl3-nx port was not used |
| Main thread, start | `sdl_host.c` | `port/nx/src/nx_host.c`: the game on its own thread, the main thread polling events and pads every 4 ms, the folder, the first dhd.ini, the GPU's official 460.8 MHz mode |
| OS services | `host_posix.c` | `port/nx/src/host_nx.c` |
| Pads | `gamepad.c` | `port/nx/src/gamepad_nx.c` (buttons by position, as on a DualShock 2) |
| Guest memory | `guest_memory_posix.c` | `port/host64/guest_memory_nx.c` (below) |
| Renderer | Rust, wgpu | `port/nx/src/dhd_render_nx.c`: `dhd_render.h`'s native half in C on deko3d, after render.rs, surfaces.rs, convert.rs, scanout.rs, present.rs and zread.rs |
| Exact renderer | Rust, compute | `port/nx/src/exact_stub.c`: a stub. Native mode reads its own depth (finding 097) |
| dhd.ini | `dhd_settings` (Rust) | `port/nx/src/settings_nx.c`: the same rules and table in C |
| Online, Discord, texture PNGs | Rust | `port/nx/src/stubs_nx.c`: left out of this first version |

There is no Rust on this target. The fiber switch is the shared `fiber_arm64.c`.

### Guest memory

`port/nx/spike_memory` is a test NRO, with no game code or data, that proved each step on the
console before the port relied on it. The recipe:

1. Reserve the 8 GiB range through libnx's virtual memory manager; nothing is mapped there yet.
2. Take heap memory (`memalign`) and turn it into process code memory with
   `svcMapProcessCodeMemory`, then make it read/write with `svcSetProcessMemoryPermission`.
3. Map it at the guest address with `svcMapProcessMemory`. This can be repeated for one block,
   which is how the six RAM views share one 32 MiB block.

A commit does steps 2 and 3 for the pages not committed yet. A page that isn't committed faults
into the reporter. Horizon has no `mprotect` for these pages, so nothing can be protected: the
deferred store-image reads stay off (the fault guard says no, and `graphics.c` reads at once).

### Shaders

`port/tools/nx_shaders.py` runs `native.wgsl` through naga-cli 29.0.3 (the repository's version) to
GLSL, then uam to deko3d's DKSH, embedded as C arrays. deko3d has no pipelines, so a draw's key sets
state directly and nothing is compiled while playing.

## Bugs found on the hardware

Each of these looked right on paper and showed up only on the console.

| Symptom's cause | Fix |
|---|---|
| `dkQueueSignalFence(queue, fence, true)`'s flush is the GPU's caches, not the queue, so submitted work could sit unsent | `dkQueueFlush(queue)` after every submit (`submit`) |
| `dkCmdBufClear` in deko3d 0.5.0 rewinds to the last chunk added, which is the previous frame's and still in flight | Add this frame's own command memory straight after the clear |
| An image view kept a pointer to its image's struct, and the struct had moved by value into `grows[]` | `dkImageViewDefaults` again at the new place |
| uam loses a dynamic index into a vector's components (it reads `.x`) | `nx_pick`, a select by index, swapped into the WGSL at the two places (`nx_shaders.py`) |
| The scanout compute shader's output came out in broken blocks | `native_scanout`'s sums done by a 2D engine blit: no shader, and cheaper |
| A heap of fixed size (1 GiB of surfaces and textures), where a PC has its video memory | Below 192 MiB free the texture cache releases the textures unused the longest (`native_core.inc`, `dhdr_nx_heap_low`) |

## Speed so far

Measured by hand on the console, on stock clocks with the debug deko3d library, before the test
tool below existed. These are rough figures, with no recorded window or seed.

| Scene | Fields/s (target 60) |
|---|---:|
| Menus | about 60 |
| Title | about 33 |
| Race | about 33 |

The debug library makes these an upper bound on the cost, not a settled figure: compare again with
the release library (`-ldeko3d`) before drawing conclusions about the renderer.

## The test tool

`port/tools/nx_fulltest.py` runs one unattended test and writes `build/nx-test/<timestamp>/report.md`
with raw logs and PNG captures. Each phase is its own launch: preflight and upload, title and menus,
race benchmark (`bench_race.py`'s route), memory card save and load, a 5000-field state trace
against `build/state-trace/mac-race.tsv`, and a check of crash reports. The player's dhd.ini is
restored and read back at the end, also after an error or Ctrl+C.

**`DHD_SETTINGS=none` can't be used as on a PC.** The console's environment comes only from dhd.ini,
and once that file exists `settings_nx.c` gives every key in its table the player's default. So the
tool writes what "no file" means as lines: `DHD_WIDESCREEN = 0` and `DHD_ASPECT = 4:3`. The table's
other defaults equal what the code does when they are unset, read from each `getenv` site, not
measured: fps 60, scale 2, sharp font, vibration on, textures on. A `DHD_SETTINGS = none` line still
goes in. The file has been read by the time it applies, and from then on `game_options` neither
reads nor writes the file, as in the recipe.

The trace's reference comes from a test-only Intel Mac build, so a match shows agreement with that
build, not with retail. `DHD_EXACT_PROBE=1` asks the stub for the lens-flare Z probes (first at about
field 1557), so the trace may legitimately diverge after that. The report gives pass/fail up to that
field and for the whole run separately.

Under nxlink the game's stderr goes to the stream and `dhd.log` is not written. A launch by hand
writes `dhd.log` and no stream.

## Sources

- [nx-mod/halo3-nx](https://github.com/nx-mod/halo3-nx), which now redirects to
  [nx-mod/xbox360-nx](https://github.com/nx-mod/xbox360-nx): Xbox 360 games on the Switch with a
  deko3d renderer. GitHub lists it as GPL-3.0.
- [nx-mod/rexglue-sdk-nx](https://github.com/nx-mod/rexglue-sdk-nx): its Switch runtime. Its LICENSE
  is BSD-3-Clause text (copyright Tom Clay, with portions from the Xenia project).
- [libnx](https://github.com/switchbrew/libnx), [deko3d](https://github.com/devkitPro/deko3d),
  [uam](https://github.com/devkitPro/uam), and naga from [wgpu](https://github.com/gfx-rs/wgpu).

Both nx-mod projects were used as references for how such a port stands on Horizon and deko3d. If
any code is adapted from them later, keep its notice and say so here, as `AGENTS.md` asks.

## Not checked / open

- `nx_fulltest.py` has run only against a local stand-in FTP server and nxlink, never on the console.
  So these are untested: its reading of sphaira's real FTP listings, whether the server takes `REST`
  on a download (the tool falls back to reading the whole NRO back), and its log parsing of real
  output.
- Whether the memory card route and the state trace recipe work on the Switch at all.
- `DHD_HEADLESS=1` on the console: the trace recipe sets it, and no run has shown what it does there.
- Whether a state trace agrees with the Intel Mac reference, before or after the first probe.
- Speed with the release deko3d library, and with sys-clk or other clocks.
- Thermals: nothing reads them, so the report only has what appears in the logs.
- Where sphaira stores an NRO sent by nxlink. The test also uploads it by FTP to the game's folder.
- Sound, pads in play, sleep and resume, and the home menu have had no systematic test.

## Texture descriptors and clocks (race benchmark, fields 6000-7000)

The renderer had 2048 image descriptors while the game's texture cache holds up to
`NATIVE_TEXTURES` (4096). Once they ran out, image creation failed and the `gs` thread's CPU time
climbed. With 8192 descriptors "no room" no longer appears in the log.

| Run | Clocks, CPU/GPU/memory MHz | fields/s | `gs` ms per field | game ms per field | `vu` ms per field |
|---|---|---:|---:|---:|---:|
| 2048 descriptors | not logged, probably 1020/460/1331 | 11.2 | 85.6 | 40.8 | 31.2 |
| 8192 descriptors | 1785/921/1600 | 46.1 | 14.3 | 21.4 | 18.5 |

Both the descriptors and the clocks changed between these runs, so the gain is not split between
them. The clocks only reach 1785/921/1600 through a sys-clk profile on the official charger; the
log showed 1020/460/1331 on battery. `unsupported_textures` was 21372 in both runs, so it does not
count these failures.

The limit is now the game thread, then `vu`: both are over the 16.7 ms a field has at 60 fields/s.

## Profile-guided optimisation (GCC, Switch)

`build_host64.py --pgo-generate` and `--pgo-use FOLDER` now work on `switch-arm64`. Only the game's
code (`core_*.c`) and `vu0_interp.c` carry the profile, for three reasons found on the console:

- With every file instrumented, the game stopped after its first log line and showed a black screen.
- libgcov's indirect call profiler reads TLS through `tpidr_el0`, which is 0 here, and faulted at
  0x4e8. `port/nx/pgo/gcov_tls.c` replaces it with a `-mtp=soft` build; link it into the training build.
- libgcov could not create its folders under `sdmc:`, so nothing was saved ("profiling:...:Skip").
  devkitA64's libgcov has no `mkdir` at all (its `_gcov.o` imports none), and each `.gcda` goes to
  `sdmc:/dhd-pgo/` followed by the object's full path in the build, unmangled. A training build now
  makes that folder itself before it writes the profile (`nx_host.c`'s `nx_profile_folders`, given
  the object folder by `build_host64.py`). The using build must have the same object folder (the
  same `--out`), or gcc finds no profile.

Race benchmark, fields 6000-7000, official charger, 1785/921/1600 MHz:

| Build | fields/s | p50 ms | p99 ms | max ms | game ms per field | `vu` ms per field | `gs` ms per field | sound underruns |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| No profile | 46.1 | 21.7 | 25.3 | 29.4 | 21.4 | 18.5 | 14.3 | 19 |
| With profile | 52.9 | 17.4 | 24.4 | 61.7 | 17.3 | 17.6 | 14.1 | 275 |

Resolution scale, same race and clocks, with the profile: scale 1 gave 52.9 fields/s (p50 17.4 ms),
scale 2 gave 52.2 (p50 18.1 ms), so at 921 MHz the GPU is not what limits the race. The GPU wait
reached about 16 ms a field in some menu scenes at scale 2. Scale 2 is not measured on battery
(460 MHz GPU), so a new `dhd.ini` still starts at scale 1.

On a Switch, A is now Cross and B is Circle.

## Where the race's time goes (DHD_NX_SAMPLE, port/tools/nx_samples.py)

Sampling each busy thread about every millisecond, waits included, in fields 6100-6900 at scale 2:
the game thread waits about 13% of the time, more in slow fields, for the graphics threads; VU spends
its time in geometry and clipping; `gs` in `native_record`, `gs_feed` and `gs_write`. Sound mixing on the
game thread is about 7%. The three longest fields, 55 to 72 ms, were at the same fields (6223, 6557,
6647) in both runs, so they come from the scripted race.

`native_clip_run` no longer zeroes its whole local `Vu` on every call: VU went from 17.7 to 17.1 ms per
field. `DHD_SPIN_US=2000` gave no clear gain (53.0 fields/s, unsampled, against 52.7, sampled) for about
1 ms more game thread CPU per field, so it stays off.

## Official clocks only

The sys-clk profile for this title uses CPU 1785 MHz in every mode, memory 1600 MHz, GPU 768 MHz
docked or on the official charger and 460 MHz on battery. Race benchmark, final profile build:

| Case | Clocks CPU/GPU/memory | Scale | fields/s | p50 ms | max ms |
|---|---|---:|---:|---:|---:|
| Official charger | 1785/768/1600 | 2 | 47.9 | 20.7 | 73.1 |
| Battery | 1785/460/1600 | 1 | 54.0 | 17.0 | 58.3 |

At scale 1 even 460 MHz does not limit the race; at scale 2 even 768 MHz does. In handheld
`nx_host.c` draws at scale 1 whatever `scale` says; `scale` applies docked.

Sphaira crashed three times just as it started an NRO (result 0, the same stack each time), before the
game's first log line. Not explained yet; the test tool polls sphaira's FTP during those launches.

## EXIT GAME returns to the Homebrew Menu (2026-11-16, not yet run on a console)

EXIT GAME used to end the whole process, taking the Homebrew Menu (sphaira) with it. Now it ends
the game's threads (host_end, finding 210); the game thread's last exit step (`last_exit_step`,
`nx_host.c`) ends that thread, `sdl_host_main` returns from `main`, and the loader reloads the menu
(exit mode 0). The system's own close request (Home, close the software) still ends the process, and
the 2 s deadline (`force_leave`) still ends it if the game does not stop. Untested: it relies on
`host_end` having stopped every other thread, which only a console run shows.

`nx_shaders.py` was adapted to main's `native.wgsl`: the depth loads are patched where they now are
(`grow_source`, `convert_depth0..3`), and the convert entry point keeps the Switch's float output
for main's `u32` stores (`nx_unpack`). The shaders compile with naga and uam; the C does not yet build
(the 0.8.9 build was stopped for memory).
