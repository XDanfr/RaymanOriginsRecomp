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

See [SWITCH_STATUS.md](SWITCH_STATUS.md) for the living checklist.
