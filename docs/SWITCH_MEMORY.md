# Nintendo Switch memory model

## Problem

The Xbox 360 executable expects a guest address space that does not map directly onto a normal desktop process.

The Rayman runtime currently provides guest memory on the host. The Switch port needs to preserve the guest-visible layout while working within Horizon's virtual-memory and heap model.

This is one of the highest-risk parts of the port.

## Reference approach

MarathonRecomp-NX uses Switch process-memory SVCs exposed by libnx to map guest regions into the process address space.

The relevant Horizon operations used by that project include:

- svcMapProcessMemory
- svcUnmapProcessMemory
- svcMapProcessCodeMemory
- svcUnmapProcessCodeMemory

It also locates a large virtual address region with libnx virtual-memory helpers before committing mappings.

The exact implementation should be treated as a reference, not copied wholesale. Rayman's guest layout and runtime allocation patterns need to be measured first.

## Questions to answer

### Address reservation

- What virtual address range does Rayman currently reserve?
- How large is the guest address space?
- Which ranges are actually touched during startup?
- Does the recompiled code contain absolute guest addresses that require fixed placement?

### Backing storage

- Which regions need committed physical backing?
- Which regions can remain sparse?
- Can heap-backed mappings satisfy the current allocator?
- Does the runtime need executable mappings for generated/recompiled code?

### Allocation

Audit use of:

- mmap
- mprotect
- munmap
- executable memory
- guard pages
- page-size constants

Any Switch implementation should preserve the semantics expected by the recompiled code rather than merely replacing calls one-for-one.

## Recommended implementation boundary

    guest allocator
         |
         v
    platform memory provider
      +-- desktop
      +-- Android
      +-- Switch/libnx

The platform implementation should own Horizon-specific virtual-memory operations.

## Important constraint

Do not reserve the entire expected guest range blindly and only discover later that the application's heap has been starved.

Other Switch XenonRecomp ports have adjusted the default application heap because guest mappings and backing allocations consume the same physical memory budget.

The final heap split should be derived from actual Rayman startup requirements and measured on hardware.

## First experiment

The first useful memory experiment is not booting the whole game.

Build a tiny Switch test executable that:

1. starts under libnx
2. finds a suitably aligned virtual address range
3. maps a small guest window
4. writes and reads through it
5. unmaps it
6. repeats with increasingly large regions

Then integrate the same primitive into the Rayman runtime.
