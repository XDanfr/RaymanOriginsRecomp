# Nintendo Switch status

Living checklist for the Rayman Origins Recompiled Switch port.

Legend:
- [ ] not started
- [~] in progress / partially working
- [x] working
- [!] blocked or needs investigation

## Repository and build

- [x] Switch branch created
- [x] Switch research documented
- [x] Switch architecture documented
- [x] Switch memory notes documented
- [x] Switch renderer notes documented
- [x] devkitA64 toolchain integrated into CMake
- [~] host-only generation workflow kept separate from the Switch target
- [x] Switch CMake configure succeeds
- [x] Switch target compiles: the generated game library and Switch runtime core now build
- [x] NRO packaging target emits `RaymanOrigins.nro` with NACP metadata and icon
- [x] NRO launch verified on hardware
- [~] reproducible Switch build instructions

## Recompilation

- [x] Rayman-specific XenonRecomp branch confirmed suitable for AArch64 Switch
- [x] ARM64 recompilation succeeds using the current configuration
- [x] generated PPC source is accepted by devkitA64
- [ ] Switch-specific XenonRecomp changes identified
- [ ] custom Switch XenonRecomp fork created only if required

## Runtime

- [x] Switch application entry point
- [x] sparse 4 GB guest-memory reservation
- [x] dynamic guest page mapping/commit and guest-addressed read/write probe
- [ ] page protection handling
- [x] main guest-thread creation and PPC context bootstrap
- [~] game-created thread lifecycle and stress behaviour (devkitA64 `pthread_detach` is unavailable; handles are retained)
- [ ] thread priority / affinity policy
- [ ] synchronization primitives
- [ ] high-resolution timing
- [~] filesystem paths (game and writable data roots selected; hardware validation pending)
- [x] file reads (complete extracted-disc startup validated on Switch hardware)
- [ ] file mapping if required
- [x] default XEX image decode/load and generated entry-point mapping
- [~] XEX/XAM assumptions audited
- [~] XEX kernel-variable import relocation (all imported variables receive guest storage; semantic values remain to be audited)
- [~] crash/exception handling (libnx exception dump written to `sdmc:/switch/RaymanOriginsRecomp/crash.log`; subsystem diagnostics written to `runtime.log`)
- [ ] unresolved import behaviour audited

The current Switch executable is a bring-up launcher. Hardware validation confirmed that it starts through libnx, reserves a sparse 4 GB guest window, commits the generated image/lookup region plus a small dynamic test page, performs a guest-addressed read/write, initializes the sparse runtime heap, and creates a host pthread with a valid PPC PCR/TLS/TEB and guest stack. Before entering the guest it now verifies that several essential files exist beside `default.xex`; deploy the complete contents of the user's own extracted disc under `sdmc:/switch/RaymanOriginsRecomp/game/`, not only the XEX.

The launcher was validated on HOS 22.5.0 with Atmosphère 1.11.2 in full-application mode. When using Sphaira through a forwarder, recreate the forwarder after updating Sphaira so its embedded NRO loader includes the current launch fixes.

## Input

- [ ] libnx HID backend
- [ ] controller enumeration
- [ ] buttons
- [ ] sticks
- [ ] triggers
- [ ] controller reconnect behaviour
- [ ] keyboard/desktop assumptions removed from Switch path

## Audio

- [ ] Switch audio backend selected
- [ ] audio device opens
- [ ] audio callback/pump runs
- [ ] timing is stable
- [ ] underruns investigated
- [ ] game audio reaches speakers/headphones

## Graphics

- [x] existing native renderer compiles for Switch
- [x] all 34 shaders observed in the sustained Switch trace have valid local SPIR-V output
- [x] optional NVK package contract and Rayman draw/clear/resolve/present bridge implemented
- [x] NVK-enabled Switch ELF and NRO link against an external Mesa 25.0.7 NVK package
- [x] Vulkan loader path works
- [x] NVK is detected (`NVIDIA Tegra X1 (NVK GM20B)` on hardware)
- [x] Switch VI presentation path (animated libnx software-framebuffer probe validated on hardware)
- [ ] swapchain created
- [x] native renderer hook boundary (shader/draw/clear/resolve/present audit validated on hardware)
- [ ] Vulkan shaders load
- [x] first diagnostic frame presented
- [ ] first Rayman renderer frame presented
- [ ] menus render
- [ ] gameplay renders
- [ ] render-to-texture paths audited
- [ ] video/movie path audited

## Game boot milestones

- [x] NRO packages and launches on hardware
- [~] runtime initializes (memory and XEX bootstrap only)
- [~] game executable is found and loaded
- [x] generated recompiled entry-point mapping verified
- [x] guest thread and PPC context initialize on hardware
- [x] guest thread calls the recompiled entry point and remains active through thousands of frame-loop iterations
- [ ] menu/home screen appears
- [ ] first level loads
- [ ] player can move
- [ ] player can die/restart
- [ ] audio plays
- [ ] rendering remains stable for 10+ minutes

The validated diagnostic run reached frame 2,700 and captured 252,915 high-level
draw calls using 34 known shaders, with no unknown-shader draws or crash. The
software framebuffer remained responsive throughout. The zeroed sampled guest
front buffer is expected while the command processor remains a null backend.

## Performance

- [ ] stable 30 FPS baseline
- [ ] stable 60 FPS baseline
- [ ] frame pacing measured
- [ ] memory usage measured
- [ ] startup time measured
- [ ] shader compilation behaviour measured
- [ ] CPU hotspots profiled
- [ ] GPU hotspots profiled

## Release hygiene

- [ ] no game code/assets committed
- [ ] private game directory remains ignored
- [x] Switch-specific build output remains ignored
- [ ] license notices preserved for reused code
- [ ] user-facing Switch documentation added
