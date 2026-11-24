# 270: Switch speed: bring the port up to main, then cut CPU work

Status: in progress

Branch `perf/switch-270` from `main` bbe4acc5 (release 0.8.10), planned 2026-11-16; merged into the
fork's `main` on 2026-11-20 with step 0, the first console runs and the return to the Homebrew Menu.
The rest continues on branches from that merge, each keeping this file current: `switch/baseline`
(steps 1 and 2: the console baseline, threads and cores, the sound underruns), `switch/mesh-path`
(step 3: Native VU's mesh draws and main's renderer gains in deko3d), `switch/pacing` (step 5),
`switch/exit-error` (the error applet seen after the return to the menu) and `switch/controls`
(rebinding, `controls.rs`'s part in C).
This file is the lane's plan while it runs and its finding when it merges. Someone who has only
this file and the git log must be able to pick the lane up.

## Goal and acceptance

The Switch homebrew build (a `switch-arm64` target, one commit `58402d62` on a contributor's
`switch-homebrew` branch, based on 0.7.6 and not yet in this repository) holds 60 fields a second
in a race. Today finding 127 on that branch measures 47.9 docked at scale 2 and 54.0 handheld at
scale 1.

Acceptance, each number measured on a named console with the clocks written down:

- Race fields 6000-7000 on `bench_race.py`'s route: fields/s, p50, p99 and max field time, before
  and after every kept change.
- Boot (600 fields) and race (5000 fields) state traces equal to the base build's. The branch's
  reference trace comes from a test-only Intel Mac build, so a match shows agreement with that
  build, not with retail; say so in the result.
- `python tools/check.py quick`, `port` and `docs`.
- A change is kept only when the whole race gains about 3 % or more, clearly outside the runs'
  spread (the rule of findings 247 and 249).

Out of scope: gameplay, the picture's content, online, Discord and texture packs on the Switch
(left out of the first port). Switch releases are published on the fork (`docs/BUILDING.md`,
"Switch releases"), the first, `v0.8.10-switch.1`, before this lane's goal of 60 is met.

## Where the port stands (finding 127 on the branch)

Built so far only on an Intel Mac. The target uses devkitA64's GCC and libnx, a deko3d renderer in
C (`port/nx/src/dhd_render_nx.c`, a copy of 0.7.6's Rust native renderer), an SDL3 shim on libnx,
and `port/nx/src/settings_nx.c` in place of the Rust settings crate. `port/tools/nx_fulltest.py`
runs an unattended test; it has never run on a console.

| Case | CPU / GPU / memory (MHz) | Scale | Fields/s | p50 (ms) | Max (ms) |
|---|---|---|---|---|---|
| Docked or official charger | 1785 / 768 / 1600 | 2 | 47.9 | 20.7 | 73.1 |
| Handheld | 1785 / 460 / 1600 | 1 | 54.0 | 17.0 | 58.3 |

Per field in a race (profile-guided build, 1785/921/1600 MHz): game thread 17.3 ms, `vu` 17.6 ms,
`gs` 14.1 ms, against a 16.7 ms budget. At scale 1 even a 460 MHz GPU does not limit; at scale 2
even 768 MHz does. The profile build logged 275 sound underruns. Homebrew has three usable CPU
cores (to check on the console), so the game, `vu`, `gs`, render, sound and main threads compete.

## Steps

0. **Bring the branch up to `main`.** Merge it on a new branch, resolve the conflicts (a dry-run
   merge on 2026-11-16 conflicts in `docs/STATE.md`, `port/gs/native_core.inc`,
   `port/gs/present.c`, `port/hle/host.c`, `port/tools/build_host64.py`,
   `port/tools/check_rust_lib.py` and `port/tools/rustgs.py`) and renumber the branch's finding
   (its 127 is taken here by `127-phone-t612-throttle.md`). This brings Native VU (249, 253,
   264), the frame-budget profile (257), stutter fixes (256) and the high-fps sweep (255).
   Native VU's effect on the Switch is unmeasured. Check the other targets still build and the
   state traces still match.
1. **A trustworthy baseline on the console.** Run `nx_fulltest.py` on real hardware, with the
   release deko3d library (`-ldeko3d`, not the debug one), fixed clocks and `bench_race.py`'s
   route. Report per-thread CPU, waits and p99/max field times, as finding 257 does on Android.
2. **Cut CPU work and fix the core layout.** Pin and prioritise threads for three cores. Move
   sound mixing (about 7 % of the game thread) off it and explain the underruns. Confirm the VU
   kernels use NEON. Extend profile-guided optimisation past `core_*.c` and `vu0_interp.c` (an
   instrumented host hung at start; find why). Try LTO and `-mcpu=cortex-a57`.
3. **Port main's renderer gains to `dhd_render_nx.c`.** Main has since added fewer render passes
   (112), a framebuffer cache (109), presenting from the display surface (247), a cheaper mesh
   path (249) and an early-Z probe change (111). The shared C parts in `port/gs` arrive with step
   0; the Rust-only parts must be hand-ported, one at a time. deko3d has no pipelines, Horizon has
   no `mprotect` for guest pages, and dual-source blending is always on.
4. **Switch device defaults** (the pattern of finding 263). Handheld starts at scale 1 with
   Performance Mode on (shadows, lens flare and particles off); docked starts at scale 2 only if
   measured to hold 60. An existing `dhd.ini` is never changed. The rule lives where the phone
   rules do, or in `settings_nx.c` if the Rust crate stays off this target.
5. **Pacing.** A steady 30 fields/s option or fallback, so play is smooth when 60 is out of reach,
   instead of swinging between 47 and 54 with 60-70 ms spikes. The three longest fields in the
   branch's runs (6223, 6557, 6647) came from the scripted race, so check what they are first.

## Investigation results (2026-11-16, read from the merged tree; nothing measured)

**B. CPU and build flags.** Threads (`sdl_nx.c` `placements`): game 0x2C on core 0 only, vu 0x2C on
core 1 only, native 0x2D and gs 0x2E on core 2 with all three cores allowed, `nx audio` 0x2A on
core 2 only, movie 0x3B. So the layout step 2 asks for already exists in the branch; what is
unknown is whether it helps (no per-thread waits are logged). The VU0 interpreter uses
`<arm_neon.h>` with SIMDe's SSE mapping, the same file as Android, so NEON is in use for it; the
emulated VU1 kernels on this target were not checked for NEON (the Switch flags are
`-march=armv8-a+crc+crypto -mtune=cortex-a57`, no `-mcpu`, no LTO). PGO is gcc's, limited to
`core_*.c` and `vu0_interp.c` (`build_host64.py`). Sound: the game's mixer writes into a 64 KiB
ring through `SDL_PutAudioStreamData`; `nx audio` only drains it, so the mixing stays on the game
thread, as the plan says.

**Native VU is not active on the Switch, and the dhd.ini table is behind.** `dhd_render_nx.c` has
no mesh hook (`gs_mesh_hook`, `gs_mesh_mode`, finding 249 and `gs_mesh.h`), so the translated mesh
loops never run and VU1 stays emulated: that is the 17.6 ms `vu` thread. A mesh path in deko3d is
therefore the largest CPU lever and belongs to step 3 first, not last. Also `settings_nx.c`'s table
lacks `native_vu`, `shadows`, `lens_flare`, `particles`, the `touch_*` keys, `language`,
`room_region` and `stick_dead_zone`, so Performance Mode cannot be set from dhd.ini on the console
until they are added.

**C. Renderer gap** (main's gains against `dhd_render_nx.c`, a copy of 0.7.6's Rust):

| Change | Applies to deko3d | Where | Effort | Risk |
|---|---|---|---|---|
| Native VU mesh draws (249, 253, 264) | yes, the biggest: removes most `vu` thread time | Rust mesh path to port; `gs_mesh.h` and `vu1_mesh.inc` are shared C | high | high: new vertex format, near-bound clipping |
| Fewer render passes (112) | yes | Rust-only | medium | medium |
| Framebuffer cache (109) | partly: Vulkan-specific, deko3d has no render-pass objects | Rust-only | medium | medium |
| Early-Z probe change (111) | yes, if the probe is used at all (the exact front end is a stub) | shared C and Rust | low | low |
| Present from the display surface (247) | to check | `present.c` is shared; the image path is Rust-only | medium | medium |

**D. Defaults and pacing.** Handheld detection is done once in `nx_host.c` (`appletGetOperationMode`
forces `DHD_SCALE=1` for that run, dhd.ini untouched), so scale already follows the mode. The
phone rule of finding 263 is Rust (`phone_defaults.rs`, `dhd_settings::phone`), which this target
does not link, so a Switch rule goes in `settings_nx.c` after the table is brought up to date, and
writes Performance Mode only to a dhd.ini that has none of `shadows`, `lens_flare`, `particles`.
A 30 fields/s option exists as the game's `fps` setting (`fps.c`, default 60, `DHD_FPS`); whether
30 paces evenly on this target needs a console run.

## Work split (subagents)

Pieces A to D are done (A: the merge, B to D: the results above). Only E remains, and it needs a console. Step 0 came first and alone, because every other piece reads the merged tree. After
it, steps 2 to 5's investigations are read-only and can run in parallel; changes follow one at a
time, each measured on the console, which only a person with the hardware can do.

| Piece | Kind | Brief | Depends on |
|---|---|---|---|
| A. Merge | writes, in its own worktree | Step 0: merge, resolve conflicts, renumber the finding, run the checks that run offline. Cannot build the NRO or run on a console; must say so | none |
| B. CPU and build flags | read-only | Every thread the Switch build creates with priority, stack and affinity; where sound mixes; whether the VU kernels use NEON; compile flags and PGO scope; whether Native VU is on | A |
| C. Renderer gap | read-only | A table of main's renderer and GS changes since 0.7.6: gain, whether it applies to deko3d, shared versus Rust-only, effort, risk, and `dhd_render.h` API changes the nx file must meet | A |
| D. Defaults and pacing | read-only | Where a Switch device rule would live, how docked/handheld is detected, files and tests to change; what a fixed 30 fields/s option needs | A |
| E. Measurement | person with a console | Steps 1 and every before/after number | A |

A first attempt at starting A to D was stopped before it produced a result; A left an unfinished
merge in a throwaway worktree. Nothing from it is used here.

## Progress

- [x] 2026-11-16: plan written (this file)
- [x] 2026-11-16: step 0 merged on branch `perf/switch-270` (first `switch/rebase-0.8.9`, on 0.8.9; worktree `.claude/worktrees/agent-a08dc94462a426a91`,
  `git merge`, the contributor's commit kept). The branch's finding became 272. Conflicts resolved:
  `host.c` (main's `host_threads.c` now carries the Switch's thread handles and CPU ticks; the SDL
  shim gained `SDL_GetTLS`/`SDL_SetTLS`), `present.c` (the Switch keeps its swapchain in the
  background but takes main's render hold, `gs_render_leave`/`gs_render_release`),
  `build_host64.py`, `rustgs.py` and `check_rust_lib.py` (macOS x64 and iOS targets both kept,
  Switch PGO beside main's clang PGO), `native_core.inc` and `STATE.md` (no markers left).
  Run here: `check.py docs` OK. NOT run: `check.py quick` stops at a missing `asm/` (no ISO on
  this machine); the NRO cannot be built (no devkitPro). The merged Switch code is uncompiled.
- [x] 2026-11-16: investigations B, C, D written above
- [x] 2026-11-16: step 4, code part: `settings_nx.c` has main's missing keys (native_vu stays off until
  the mesh path exists); a new dhd.ini starts with shadows, lens flare and particles off. Neither is
  compiled or measured; a docked scale 2 default is not set, as it is not yet shown to hold 60.
- [x] 2026-11-17: main's 0.8.10 merged in (`0287436c`); the Switch findings renumbered to 270 (plan) and 272 (port), because 267 and 268 went to the iOS A12 and touch left/right findings. `check.py docs` OK; `quick` fails here only on a missing `llvm-nm` and a wasi-sdk path for another OS. The branch is pushed to the contributor's fork, not to the main repository.
- [x] 2026-11-17: the merged Switch code compiled for the first time (devkitA64 15 from the `devkitpro/devkita64` Docker image,
  `-fsyntax-only` over every unit of `host_units()` plus `port/host64`'s glue, implicit declarations as errors; naga-cli 29.0.3 and
  uam made the shaders from main's WGSL). It found, and this lane fixed: `stubs_nx.c`'s `dhd_disc_check` lacked main's `stop` and
  `pause_us`; `dhd_render_nx.c`'s `dhdr_surface_counter` lacked the riders' time and `dhdr_surface_touch` was missing; the SDL shim
  lacked what main's sound (pause, clear, gain, destroy), window state, `SDL_InitState`, `SDL_AddAtomicInt` and finger events now
  use (the audio thread now handles pause, clear, mute and close itself); `touch_host.c` is replaced on the Switch by `touch_nx.c`
  (no touch controls yet, as a desktop). NOT done: the link, the NRO and any run, which need the disc (the wasm core) and a console.
- [x] 2026-11-17: first full NRO build attempt, on Windows in Docker (an image from `devkitpro/devkita64`
  with wasi-sdk 34, WABT 1.0.42, SIMDe and naga-cli 29.0.3; the source in a Docker volume, as a bind
  mount from the Windows drive is slow). `vu0_interp.c` alone takes over 15 minutes under devkitA64's
  gcc -O3 and hit the host units' 900 s timeout; the Switch's host units now get 7200 s
  (`build_host64.py`). `--jobs 8` fits gcc's ~3.5 GB per core chunk in 32 GB. Off Windows the object
  cache works only with `DHD_OBJCACHE_DIR` set (`objcache.py`); without it every build recompiles all.
  The first link named what main gained since 0.7.6 and the Switch's C lacked. Now: controllers
  give main's raw state (`gamepad_nx.c`; A stays Cross, B Circle; PS2 symbols, `PAD_KIND_NONE`),
  through the layout code moved out of `gamepad.c` into `gamepad_layout.c` for every platform;
  `settings_nx.c` has the install id and the default layouts, but no rebinding (the CONTROLS page
  answers "Not on the Switch yet"; porting `controls.rs` is open); `dhd_render_nx.c` answers no
  device loss, no mesh draws, no GPU timings, no pipeline list, no surface read back; the race ping
  and the network cut are stubs as the rest of online.
- [x] 2026-11-18: the NRO's first runs on a console (sphaira, nxlink, 1785/768/1600 MHz, scale 1).
  The title's 3D scene came out as grey shards and missing blocks at 27 fields/s. Cause: ZCULL keeps
  coarse Z per tile for the bound Z image and drops it only when the bound address changes, so a Z
  surface written elsewhere since (another pass, a copy, the probe) left stale tiles that rejected
  whole blocks of the scene. `dhd_render_nx.c` now flushes pending draws and invalidates ZCULL after
  binding a target with depth; the scene draws, at 44-46 fields/s. Also added: the image descriptor
  cache is invalidated before any draw or dispatch that samples, when a descriptor was written since
  (a freed slot given to a new image could still read as the old one); it changed nothing visible,
  but deko3d requires it. Test tools kept: `DHD_NX_DRAWS=N` writes picture N's commands to
  `draws_N.csv`, and a log line names a display surface smaller than the picture.
- [x] 2026-11-18: menus and rider select still showed warp-sized blocks of wrong texels, smears, old
  text and a grey box on the logo, with `textures = off` too. Bisect: with the draw stages built from
  0.7.6's `native.wgsl` and `native_dual.wgsl` (the text the contributor's port drew correctly with)
  and everything else from main, the main menu and rider select are clean. Main's draw shaders as of
  `fb12874d` are already broken; that commit adds a `vec2` select, `round()` and a branch around the
  texel load, and HEAD adds a `vec4` select with `clamp` in `load_texel` and the alpha test before
  `discard`. Removing `fb12874d`'s three did not fix HEAD. The specialisation constants
  (`native_spec`, `native_texel_scale`) come out as plain `0` in the GLSL and are not the cause. The
  blocks fit a uam miscompile around branches, `discard` and texture loads, which each version of
  main's shader trips in a different place; it was not narrowed further. See the decision below.
- [x] 2026-11-20: EXIT GAME and `DHD_NX_EXIT` return to the Homebrew Menu (phases B and C pass, no
  crash report). hbl gives the next NRO the same heap and checks nothing, so what the game left in it
  stopped hbl with `0xD401` (InvalidCurrentMemory) at `hbl + 0x4034`. `DHD_NX_MEMMAP=1`
  (`exit_memory.txt`) found three leftovers, each fixed: the game thread's 64 MiB stack (it ended with
  `svcExitThread`, so libnx's `threadClose` refused to unmap it; now `threadExit`, and a refused close
  is logged and falls back to the system's exit), the socket service's transfer memory (`0x234000`,
  libnx's default BSD config; `socketExit` was never called), and the guard pages of the EE fibers'
  8 MiB stacks (`host64_fiber_guards_release`). Before those, threads left running and guest memory
  left as code memory had stopped hbl the same way (`nx_threads_end`, `nx_guest_memory_release`).
  Open: after this run the player saw "The software was closed because an error occurred" (the error
  applet, no crash report, no erpt on the card); when it appeared and its code are not known.
- [x] 2026-11-20 (`switch/exit-error`, static reading only, not yet run on a console): that error
  without a report fits the crash handler. It ended the process with `svcExitProcess` behind the
  applet manager's back, so a fault anywhere (on the way out too, after `socketExit` took nxlink's
  stream with the report) showed exactly that message, with no code and nothing on the card. It now
  writes `crash.txt` beside dhd.log and gives the exception back to the kernel unhandled, so
  Atmosphère writes its report and the error has a code. The way out writes `exit_steps.txt`, one
  line per step, on the card at once and after nxlink's stream is gone, up to libnx's own exit
  (`userAppExit`). It ends with a probe of the loader's heap: sphaira's loader reads the next NRO with
  one read over the whole heap, which fails on any page that isn't plain read/write heap (as in
  the dw3-switch port's notes on its Sphaira exit, on GitHub as Larbaco/dw3-switch: the same `0xD401` at `hbl + 0x40xx`). A heap that
  isn't clean takes the system's exit instead of the menu. Also given back now: the `DHD_NX_SAMPLE`
  thread (made outside the shim's list, so it outlived the game), audout when it was opened but
  never started, and the audio buffers. The audio thread's untimed wait could keep it from seeing
  the close while sound was stopped out of focus. The clkrst session is closed, and the main thread
  gets the loader's priority back (the menu runs on it next). Open: the error's code and moment.
  Also open: whether sphaira alone, closed the same way, shows it. Users report that message when
  closing sphaira itself with + (GBAtemp, nton issue 12).
- [x] 2026-11-20 (`switch/controls`): button rebinding. `settings_nx.c` now has `controls.rs`'s rules in C
  (`dhd_controls_layouts`, `dhd_controls_edit`: the same dhd.ini lines `keyboard`, `pad1` to `pad4`, the
  same swaps, refusals and words on the CONTROLS page), in place of the defaults and "Not on the Switch
  yet". A host build of the file (Linux gcc, `-Wall -Wextra -Werror`, UBSan) passed a test of
  `tests/controls.rs`'s cases (scratch, not kept); the NRO builds. NOT done: a console run of the page.
  Controllers report `PAD_KIND_NONE`, so the page names buttons with Xbox's words: right for A and B
  (`gamepad_nx.c` gives A as south, B as east), swapped for X and Y (Y is west, which Xbox calls X).
  No pad kind's words fit that mapping; open.
- [x] 2026-11-20 (branch `switch/mesh-path`): the uam fault narrowed offline, without a console. A
  self-built uam (devkitPro/uam `5a5afc2`, release build) gives stock uam's bytes, and envytools'
  `envydis -m gm107` reads the code. uam writes `ShaderLocalMemoryCrsSize` (the off-chip call/return
  stack, SPH CommonWord3) as 0 for every graphics stage (`compiler_iface.cpp`: "TODO: Proper logic";
  compute gets 0x800). The on-chip stack holds 16 entries, two for each nested SSY, PBK or PCNT
  (mesa's `nvc0_program.c` has the sizing, commented out: `((maxCFDepth * 2) + 47) / 48 * 0x200`
  when `maxCFDepth * 2 > 16`). Measured over every path of the code: main's `native_pixel` and
  `native_pixel_dual` nest 11 deep; 0.7.6's pinned copy and main's `native_convert` nest 7; every
  other stage nests 0-1. Every stage that draws right on the console nests 8 or less, and main's
  broken draw stages nest deeper. A warp that overflows the stack loses where it reconverges, so its
  pixels come out wrong as a block. `nx_shaders.py` now measures the nesting (`branch_depth`) and
  gives a stage deeper than 8 the warp's scratch as its stack (`with_crs`: 0x800 bytes, the queue's
  default, in the SPH and in `per_warp_scratch_sz`). This changes no stage that ships today (all 7 or
  less). `NX_DRAWS=main` builds the draw stages from main's `native.wgsl` for a console run.
  Not explained: `fb12874d`'s draw stages nest only 5 and were reported broken too. That commit
  brings 20 more `f2f ... round` (`round()`) and a branch around the texel load, so a second fault
  may remain. `native_convert` also uses `round()` and draws right.
  Sources: mesa `src/gallium/drivers/nouveau/nvc0/nvc0_program.c` and `nvc0_screen.c`, NVIDIA's
  open-gpu-doc "Shader Program Header" (CRS size in bytes per warp, multiples of 512), envytools, and
  devkitPro/uam pull request 4 (a claim against its dual-issue pass, closed unmerged).
- [x] 2026-11-20 (branch `switch/mesh-path`): step 3's first part, Native VU's mesh draws in deko3d.
  `nx_shaders.py` builds `native_mesh_vertex` from main's `native.wgsl` (it has no branches, and its
  outputs are the pinned pixel stages' inputs). `dhd_render_nx.c` binds it with the mesh vertex
  layout (`mesh_attributes`, pipelines.rs's `MESH_ATTRIBUTES`) for a draw with flag 4096 (key bit 24).
  It culls back faces unless flag 8192 is set (key bit 25; `raster_bind`, and every full-screen pass
  sets culling off again). `dhdr_native_meshes()` now answers yes. The rest is the shared C
  (`native_core.inc`'s `native_mesh`, `vu1_mesh.inc`). `native_vu` stays off by default in
  `settings_nx.c` until a console run shows the meshes right. Open for that run: the front face
  (deko3d's CCW with `OriginUpperLeft` is assumed to cull what wgpu's `Back` culls).
- [ ] step 1: baseline on the console
- [ ] steps 2 to 5
- [ ] 2026-11-20, step 5 (`switch/pacing`, built, not yet run on the console): at `fps = 30` the
  game paced its frames on a wall-clock grid of 1/30 s (`kernel.c` `pace`) while the swapchain
  showed each picture at the next vblank (swap interval 1), so the two clocks met at a drifting
  phase and a picture could stay one, two or three vblanks. `dhd_render_nx.c` now sets the
  swapchain's interval at each present from `port_frame_period()` in whole vblanks (1 to 2): 2 at
  `fps = 30` in a race, 1 from the load to GO (the grid's 1/60) and at 60, 0 with `vsync = 0`. The
  game clock is unchanged (finding 139: no fixed step). To check on the console with `DHD_FPS=30`:
  the `nx present: swap interval` lines in dhd.log, fields/s near 30 with p99 and max field times
  near 33 ms, the countdown at real speed, and the race's feel against `fps = 60`.
- [x] 2026-11-20, console runs (docked, 1785/768/1600 MHz), all merged into `main`:
  - `switch/pacing`, race at `fps = 30`: 30.000 fields/s over 6000-7000, p50 33.33 ms, p99 33.57 ms,
    max 38.9 ms (60-70 ms spikes before). Step 5 done.
  - `switch/exit-error`: its first run froze after "exit() reached its last step". The game's
    thread took the exit (`leaving`) and set `menu_exit` only after the exit step's write to the card
    (about 60 ms); the main loop parks for good on `leaving` without `menu_exit` and saw the gap. Main
    had the same gap, shorter (`flush_all`'s commit of sdmc alone): the same freeze after the pacing
    run's race. `menu_exit` is now set right after the claim. Then phases B and C both came back to
    sphaira: `exit_steps.txt` to libnx's exit in 221 ms, heap 0 regions the loader can't read over,
    no crash report. The error applet's message was not seen again; sphaira closed alone is untested.
  - `switch/controls`: by hand on the console, the page's names (now A B Y X as printed: the pages
    get `PAD_KIND_SWITCH`, `hle.h`'s `pad_input_word`), a swap, a refusal, Swap Sticks, the layout
    kept over a restart and Reset all as expected; no `controls:` problem lines in dhd.log.
  - `switch/mesh-path`, `native_vu = on`: the race's picture right (rider, terrain, trees, cliff,
    HUD; back faces culled as wgpu's), 12 loops drawn natively, the race 46.6 fields/s against 46.0
    with it off (+1.3 %, under the 3 % rule alone; the renderer gains build on it). Still off by
    default; `NX_DRAWS=main` (the uam stack workaround) not yet run.
  - `NX_DRAWS=main` (main's draw stages with the 0x800-byte stack for nesting over 8): still broken
    on the console, the menus in missing and miscoloured blocks and stripes, the race almost all
    black but the HUD (its 59.7 fields/s counts nothing). So the stack size is not the whole fault: the
    second suspect stays, `fb12874d`'s `f2f round` instructions and the branch around the texel load
    (its stages nest only 5 and were broken too). The pinned 0.7.6 copy stays.
  - Merged `main` (`1e7765e4`, every lane above): phases A-C pass, the menus' window 45.7 fields/s
    (worst field 50 ms: the bench window now free of the test captures, `fd1eddd1`), the race 52.5,
    no error lines, back to sphaira with `exit_steps.txt` to libnx's exit.
  - Surprise: build_wasm.py's core cache keys on `git ls-files`, which finds nothing in the Docker
    build volume (no checkout there), so every core build there is a cache hit: a change to the core
    (`port/hle`) needs `DHD_MODULE_CACHE=0` in that volume.
- [x] 2026-11-20: first Switch release on the fork, `v0.8.10-switch.1` (`a2dbbcec`, the merged
  main's NRO above): published as a prerelease, then made a full release the same day at the
  owner's choice, with the known gaps (no steady 60) in its notes. The steps and the open work are
  in `docs/BUILDING.md`, "Switch releases".
- [x] 2026-11-23: the target clocks are now stock, 1020/460/1331 MHz with sys-clk off (the owner's
  choice); runs with sys-clk on swung between 47 and 60 fields/s and do not count. Four lanes in
  parallel, each on its own branch from `main`, then one console session at those clocks
  (`bench_race.py`'s route, 6000-7000):

  | Build | Race fields/s | p99 ms | Picture |
  |---|---|---|---|
  | `main` (pinned 0.7.6 draw stages) | 27.9 | 43.2 | right |
  | `switch/render-gains`, both switches off | 29.2 | 43.1 | right |
  | `switch/render-gains`, conversions batched | 39.5 | 30.2 | right |
  | `switch/render-gains`, batched and direct present | 39.8 | 29.1 | right |
  | `switch/uam-fault`, main's stages flattened | 27.4 | 43.9 | blocks |
  | `switch/uam-fault`, flattened, safe schedule | 33.8 | 36.1 | right |
  | `switch/uam-fault`, main's stages, safe schedule | 37.0 | 31.6 | right |
  | `switch/combined` (the three lanes, main's stages, safe schedule) | 40.4 | 29.2 | right |

  - `switch/render-gains`: finding 112's prepared conversions now share one barrier
    (`DHD_NX_CONVERT_BATCH=0` turns it off), +35 %; finding 247's present from the display surface
    (`DHD_DIRECT_PRESENT=0`), +0.8 %, kept since it costs nothing; a barrier count in the heartbeat.
    Finding 111's probe change is gone from main (`58ee539e`) and finding 109 caches Vulkan objects
    that deko3d has none of: nothing to port.
  - `switch/uam-fault`: the second uam fault is its schedule. A Maxwell interpreter run on uam's code
    for main's `native_pixel` matched a model of the WGSL over 1200 pixels, so `f2f round` and the
    texel load compute right; the console then showed that flattening the per-pixel branches alone
    leaves blocks, and a schedule where every draw-stage instruction waits on all barriers (stall
    15, no operand reuse) draws right, and faster. `switch/combined` makes that the build
    (`draw_schedule`) and deletes the pinned copy (Decisions).
  - `switch/audio`: the underruns were the resampler's fill, `need + 2` frames for `need * ratio`;
    above about +1000 ppm each pull came 1-4 frames short, and audout's three buffers left no slack
    to hide it. Now `ceil(need * ratio) + 2` (shared host code, `port/host/sound_out_sdl.c`): 0
    underruns and 0 ms of silence in the combined race (1178 before). The bench line gains
    `mix_ms_per_tick` (about 1.5-2.2 ms) and the exit a `snd session:` line. Moving the mixer off
    the game thread is not done: its results feed game state inside the core.
- [x] 2026-11-23/24 (same clocks), `native_vu = on` with the combined build: 36.1 fields/s against
  40.4 off. The VU thread drops from about 34 to 20 ms a field, but the present waits up to 19 ms:
  at 460 MHz the GPU's mesh draws cost more than they save. It stays off.
- [x] 2026-11-24, `switch/cpu-flags` (merged into `main`, `ccbe1a05`):
  - `-mcpu=cortex-a57`: the build already passes `-march=armv8-a+crc+crypto -mtune=cortex-a57`;
    gcc's target state and two units' code are identical with it. No change.
  - LTO (`--lto all`, `--lto host`): 26.8 and 27.9 fields/s against 39.2 without, the VU and GS
    threads about twice as slow; the shaders were checked identical. Not used; why LTO loses this
    much is not known.
  - The PGO hang at start: gcc puts `__gcov_indirect_call_profiler_v4` in every instrumented
    function that can be called through a pointer, and libgcov's copy reads `tpidr_el0`, 0 on
    Horizon, so the first host function (and the crash handler's prologue) faulted.
    `port/nx/pgo/gcov_tls.c` gives the profiler its own storage; with `--pgo-scope all` the host
    units count atomically (`-fprofile-update=atomic`), the game units stay `single`. The training
    build now runs: 9 fields/s, 192 `.gcda` files at the bench's end.
  - PGO over every unit (`--pgo-use P --pgo-scope all`), against the same NRO without it, in
    alternating runs: window 28.6 against 35.2 fields/s, whole-race median 29.6 against 35.9
    (+21 %; three pairs, PGO ahead in each). The picture is right. A single run swings by up to
    30 % (the delta-time clock puts each run in a different stretch of the route, and the console
    warms over a session), so earlier single-run comparisons carry that noise.
  - The VU kernels use NEON (15-76 % of their instructions on vector registers).

## Decisions

- The merge keeps the original commit (`git merge`, not a rebase), so the contributor's authorship
  stays and the branch can be compared with the first commit.
- Clocks, the deko3d library and the route are fixed before any number counts, because finding 127
  changed descriptors and clocks together and could not split their effects.
- (2026-11-23, replaces the pinned copy of 0.7.6's draw shaders, `port/nx/native_draws.wgsl`, kept
  from 2026-11-18 as an exception to finding 099.) Every Switch stage is built from main's
  `port/gs/native.wgsl`; the draw stages (`native_vertex`, `native_pixel`, `native_pixel_dual`) get a
  hazard-free schedule (`nx_shaders.py`'s `draw_schedule`) in place of uam's, which drew warp-sized
  blocks of wrong texels. Which hazard in uam's schedule is at fault is not known; a narrower patch
  that keeps more of uam's schedule could be faster.
- The performance target is stock clocks, 1020/460/1331 MHz with sys-clk off (the owner,
  2026-11-23). Numbers at 1785/768/1600 from before that date compare only with each other.
- Switch releases are built with the profile (`--pgo-use`, every unit). The profile is never
  committed: it goes with each Switch release as `dhd-switch-pgo.zip` (about 1 MB, the
  `.gcda` files) and the next release builds from it (`docs/BUILDING.md`, "Switch releases"; the
  owner, 2026-11-24). It is made again when the hot code has changed a lot.
- A speed change counts only from alternating pairs of runs, not from one run each.

## Surprises

- The uam fault was not in the code it generates but in the header it writes: a graphics stage gets
  no off-chip call/return stack, so only stages that branch deeply fail. main's draw stages branch
  deeper than 0.7.6's (`fetch_u`, `bilinear_u`, `combine`), which is why every newer version broke in
  a different place.
- Native VU was assumed to be a thing to measure on the Switch; it is not present there at all (the
  renderer has no mesh hook). Step 3's first job is the mesh path.
- The thread placement of step 2 was already in the contributor's branch.
- The merge compiled by nobody for a day hid six API breaks (above), all from main's changes since 0.7.6 and 0.8.9; the shared code gained SDL calls the Switch's hand-written shim does not have, so every merge of main needs this compile check until the build runs in CI.
- The picture faults were not in the C renderer's reading of main's commands, as first assumed, but
  in two places outside it: ZCULL state that deko3d leaves to the program, and uam's compile of main's
  draw shaders. Neither shows on a PC; only a console run does.

## Result

Filled in at the merge.
