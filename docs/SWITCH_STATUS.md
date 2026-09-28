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
- [~] NRO packaging target added; hardware launch still needs verification
- [ ] reproducible Switch build instructions

## Recompilation

- [x] Rayman-specific XenonRecomp branch confirmed suitable for AArch64 Switch
- [x] ARM64 recompilation succeeds using the current configuration
- [x] generated PPC source is accepted by devkitA64
- [ ] Switch-specific XenonRecomp changes identified
- [ ] custom Switch XenonRecomp fork created only if required

## Runtime

- [x] Switch application entry point
- [~] guest memory reservation
- [~] guest memory mapping/commit
- [ ] page protection handling
- [ ] thread creation
- [ ] thread priority / affinity policy
- [ ] synchronization primitives
- [ ] high-resolution timing
- [ ] filesystem paths
- [ ] file reads
- [ ] file mapping if required
- [ ] XEX/XAM assumptions audited
- [ ] crash/exception handling
- [ ] unresolved import behaviour audited

The current Switch executable is a bring-up probe rather than a game launcher. It reserves a sparse 4 GB guest window, commits the generated image/lookup region plus a small dynamic test page, and performs a guest-addressed read/write before waiting for B.

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

- [ ] existing native renderer compiles for Switch
- [ ] Vulkan loader path works
- [ ] NVK is detected
- [ ] Switch VI surface created
- [ ] swapchain created
- [ ] shaders load
- [ ] first frame presented
- [ ] menus render
- [ ] gameplay renders
- [ ] render-to-texture paths audited
- [ ] video/movie path audited

## Game boot milestones

- [~] NRO launches once hardware verifies the bring-up target
- [ ] runtime initializes
- [ ] game files are found
- [ ] recompiled entry point runs
- [ ] menu/home screen appears
- [ ] first level loads
- [ ] player can move
- [ ] player can die/restart
- [ ] audio plays
- [ ] rendering remains stable for 10+ minutes

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
