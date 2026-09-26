# Nintendo Switch research

Research notes for bringing Rayman Origins Recompiled to Nintendo Switch.

## Why this should be feasible

Rayman Origins Recompiled already runs its recompiled game code natively on ARM64 and already has a native Vulkan renderer. That removes two major pieces of work normally required by a recompilation port.

The remaining problem is primarily the platform/runtime boundary:

- memory mapping
- threading and synchronization
- filesystem
- input
- audio
- window/presentation
- application startup
- packaging

## UnleashedRecomp-NX

Repository: https://github.com/NaGaa95/UnleashedRecomp-NX

The Switch port uses devkitA64, libnx, CMake cross compilation and AArch64 GCC. Its toolchain defines the Switch platform and links through libnx switch.specs.

Switch-specific operating-system code is kept in separate files instead of making every runtime implementation Switch-aware.

Its Vulkan path uses NVK and the Nintendo VI surface extension. This is particularly relevant to Rayman because Rayman's renderer is already Vulkan-native.

## MarathonRecomp-NX

Repository: https://github.com/NaGaa95/MarathonRecomp-NX

Marathon is useful for the deeper runtime work, especially memory handling. Its Switch implementation maps the large guest address space using Horizon process-memory APIs exposed by libnx instead of pretending the host provides an ordinary desktop virtual address space.

The important lesson is that guest memory layout is a first-class Switch port problem.

Marathon also demonstrates the need for Switch-specific runtime sources, custom exception handling, special memory mapping, host-only build stages and separate treatment of generation tools versus the final NRO.

## What we should copy conceptually

### Good patterns

- keep host tools native to the development machine
- keep Switch code behind a platform boundary
- use a dedicated devkitA64 toolchain file
- make the Switch target explicit in CMake
- keep NRO packaging separate from host generation
- use libnx/Horizon APIs directly where they are the correct primitive
- prefer Vulkan/NVK over introducing a second graphics backend

### Patterns not to copy blindly

- game-specific filesystem paths
- game-specific import implementations
- game-specific timing assumptions
- game-specific renderer glue
- arbitrary heap sizes
- assumptions about XAM/kernel APIs

## Expected first blockers

1. The current CMake graph builds the game directly and does not yet distinguish host generation from the final target.
2. Runtime memory currently follows desktop assumptions.
3. The current input/audio paths are built around SDL/host facilities.
4. Vulkan presentation is currently platform-specific to the existing desktop/mobile paths.
5. The final NRO needs a Switch entry point and packaging step.

## Reuse boundary

Rayman Origins Recompiled is GPL-3.0. The project README also notes that its GPL license matches Unleashed Recompiled, making compatible GPL runtime reuse a practical option where the code actually fits.

Any reused code must retain its applicable copyright and license notices.

## Research policy

Before adding a Switch compatibility workaround, prefer:

1. an existing Rayman abstraction
2. a libnx/Horizon primitive
3. a proven pattern from another XenonRecomp Switch port
4. a narrowly scoped compatibility shim

Avoid global hacks until the actual incompatibility is understood.
