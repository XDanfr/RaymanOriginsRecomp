# Nintendo Switch port

This document describes the intended Nintendo Switch port of Rayman Origins Recompiled.

The port targets Nintendo Switch through devkitA64 + libnx. The existing recompiled game code and native Vulkan renderer remain the core of the project. Switch-specific work should be isolated behind platform/runtime layers rather than turning the whole project into a Switch-only fork.

## Goals

- build Rayman Origins Recompiled for AArch64 with devkitA64
- package the result as a Switch NRO
- replace desktop OS/runtime assumptions with libnx/Horizon equivalents where required
- keep the existing ARM64 recompiled game code usable
- keep the existing Vulkan renderer and adapt its presentation path to Switch
- support Switch input, audio, timing, filesystem, threads and synchronization
- boot from a user's own legally obtained game dump

## Non-goals

- redistributing game code, binaries or assets
- emulating an Xbox 360 GPU on Switch if the native Vulkan renderer can be used
- creating a separate renderer unless the existing renderer proves incompatible
- immediately maintaining a large independent XenonRecomp fork

## Target architecture

    Xbox 360 default.xex
            |
            v
    XenonRecomp / Rayman analysis
            |
            v
    recompiled PPC -> ARM64 C++
            |
            +------------------------------+
            |                              |
            v                              v
      Rayman runtime                 native Vulkan renderer
            |                              |
            +--------------+---------------+
                           |
                           v
                    Switch/libnx layer
                           |
                           v
                       Vulkan/NVK
                           |
                           v
                      Switch display

The important distinction is that Switch is a platform target, not a second game implementation.

## Work areas

### 1. Build system

- devkitA64 CMake toolchain
- Switch-specific compile/link definitions
- host-side generation tools
- generated PPC sources accepted by the devkitA64 compiler
- Switch-side static library target for the recompiled game code
- shader generation
- NRO packaging
- eventually reproducible CI/build instructions

### 2. Runtime

The current runtime contains desktop-oriented assumptions around:

- virtual memory
- threads
- synchronization
- filesystem paths
- file I/O
- timing
- process/module handling
- crash handling
- networking/XAM stubs

These need to be audited individually. Do not assume an Xbox 360 API maps directly to one libnx call.

### 3. Memory

The recompiled game expects a large Xbox 360-style guest address space. Switch ports of other XenonRecomp projects use Horizon virtual-memory primitives to reserve/map guest regions and allocate backing memory from the application's heap.

See [SWITCH_MEMORY.md](SWITCH_MEMORY.md).

### 4. Rendering

Rayman already has a native Vulkan renderer. The first Switch renderer task is therefore presentation and integration, not rewriting the renderer.

See [SWITCH_RENDERER.md](SWITCH_RENDERER.md).

### 5. Input

The Android/SDL input abstraction should be audited for a clean Switch backend using libnx HID APIs. A platform-specific backend is preferable to scattering Switch conditionals through gameplay code.

### 6. Audio

Audio timing is likely to need special attention. Other XenonRecomp Switch ports have required platform-specific audio pump/timing work, so audio should be treated as its own compatibility area rather than assuming the desktop path will behave identically.

## Suggested source layout

    runtime/
    ├── host/
    │   └── ...
    ├── kernel/
    │   └── ...
    └── switch/
        ├── platform_switch.cpp
        ├── memory_switch.cpp
        ├── thread_switch.cpp
        ├── sync_switch.cpp
        ├── filesystem_switch.cpp
        ├── audio_switch.cpp
        ├── input_switch.cpp
        └── renderer_switch.cpp

The exact split should follow the existing Rayman abstractions once the relevant files have been audited. Avoid creating empty platform files solely for symmetry.

## Reference implementations

- [UnleashedRecomp-NX](https://github.com/NaGaa95/UnleashedRecomp-NX)
- [MarathonRecomp-NX](https://github.com/NaGaa95/MarathonRecomp-NX)

They are references for Switch/libnx integration, not forks or dependencies of this project.

Both demonstrate the general pattern of host generation tools, a dedicated devkitA64 toolchain, Switch runtime/platform sources, Vulkan on Switch and NRO packaging.

## Current state

The first Switch bootstrap milestone is complete: the retail `default.xex` expected by the project is accepted by the existing regeneration tooling, generated PPC translation units compile with devkitA64, CMake links the recompiled and Switch runtime libraries into `rayman_switch`, and `elf2nro` packages `RaymanOrigins.nro` with NACP metadata. The NRO has started on hardware and validated sparse 4 GB guest-memory reservation plus a dynamic guest-page commit/read-write probe.

The XEX/image and guest-context stages are also validated: the Switch bootstrap decodes the user's `default.xex` into guest memory, confirms that its entry point resolves to the generated `_xstart` mapping, initializes the sparse runtime heap, and creates a host pthread with a valid PPC PCR/TLS/TEB and guest stack. The current bring-up target releases that prepared thread into `_xstart`; hardware logs have shown the guest remaining active for thousands of frame-loop iterations without a runtime exception. It checks for a small set of essential data files before entering guest code and writes native exception details to `sdmc:/switch/RaymanOriginsRecomp/crash.log` if libnx catches a fault. Runtime subsystem diagnostics are redirected to the line-buffered `sdmc:/switch/RaymanOriginsRecomp/runtime.log`, since Sphaira's console does not display the runtime's `stderr` stream. XEX kernel-variable imports are relocated into persistent guest runtime storage before the entry point runs; this is required by sparse memory because desktop's fully mapped 4 GB arena had previously hidden unresolved variable tokens such as `XexExecutableModuleHandle`. The generated PPC `mftb` helper uses libnx's user-accessible `armGetSystemTick()` on Switch instead of the trapped `cntvct_el0` register. The Switch GPU backend explicitly commits its guest MMIO register window before startup writes. The 64 KB XMA register window at `0x7FEA0000` is also committed before guest startup so direct audio lock/kick accesses do not fault in the sparse address space; XMA decoding itself remains unimplemented. A static audit of all 13 generated `PPC_MM_*` operations found only the GPU (`0x7FC8xxxx`) and XMA (`0x7FEAxxxx`) windows, and both are now covered. Current devkitA64 returns `ENOSYS` for `pthread_detach`; Switch process-lifetime workers therefore keep their pthread handles instead of treating that expected shim result as startup failure. Detached guest-thread exceptions are caught and reported instead of silently invoking `std::terminate`; an actual process termination writes a short marker to `crash.log`.

The next graphics bring-up layer is also hardware-validated. On the first guest `XE_SWAP`, the main libnx thread releases the text console and creates a real 1280x720 double-buffered software framebuffer. It presents animated diagnostic colour bars and records the guest front-buffer metadata, while strong wrappers around the same high-level D3D functions used by the desktop native renderer audit shader creation, draws, clears, resolves, and presents. A sustained hardware run reached frame 2,700 with 252,915 captured draw calls, 34 known shaders, no unknown-shader draws, and no crash. The null GPU backend left the sampled Xenos front buffer zeroed, as expected. This is intentionally a presentation and hook-integrity test, not a game renderer. Rayman pixels still require the existing Vulkan renderer to be adapted to `VK_NN_vi_surface` and linked against a separately supplied Switch NVK build.

The NRO does not contain copyrighted game data. Copy the complete contents of the user's own extracted Xbox 360 disc—not only `default.xex`—to `sdmc:/switch/RaymanOriginsRecomp/game/`. In particular, the initial boot reads `localisation/localisation.loc`, `secure_fat.gf`, `bootsequence_X360.ipk`, `menus_X360.ipk`, and numerous other IPK/media files from that directory. A desktop diagnostic run with only `default.xex` reproduced a low guest-address fault; the same build with the complete extracted directory progressed through GPU/APU initialization and more than 1,500 render-loop frames.

Filesystem/process behaviour beyond that diagnostic baseline, Vulkan/NVK rendering, input, and decoded audio remain ahead. XenonRecomp output generation and basic NRO packaging are no longer the immediate blockers.

The branch should remain buildable as a Switch development target throughout this work. A successful NRO bootstrap is useful progress, but it is not yet a playable Switch port.

See [SWITCH_STATUS.md](SWITCH_STATUS.md) for the living checklist.
