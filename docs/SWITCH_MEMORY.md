# Nintendo Switch memory model

## Problem

The Xbox 360 executable expects a guest address space that does not map directly onto a normal desktop process.

The Rayman runtime now provides the first Switch-specific bridge for that model. The process reserves a 4 GB guest-address window through libnx virtual-memory helpers, while only explicitly committed regions receive backing mappings.

## Reference approach

MarathonRecomp-NX and UnleashedRecomp-NX use Switch process-memory SVCs exposed by libnx to map guest regions into the process address space.

The relevant Horizon operations are:

- svcMapProcessMemory
- svcUnmapProcessMemory
- svcMapProcessCodeMemory
- svcUnmapProcessCodeMemory

The Rayman implementation adapts the same primitive behind GuestMemory::CommitRange() instead of importing another project's memory allocator wholesale.

## Current implementation

GuestMemory::Init() now:

1. verifies that the required process-memory SVCs are available
2. finds a page-aligned 4 GB ASLR window with virtmemFindAslr()
3. reserves that window with virtmemAddReservation()
4. allocates a per-page commit bitmap
5. commits the initial low-memory region while leaving page zero unmapped
6. commits the generated XEX image/lookup region before registering PPCFuncMappings

GuestMemory::CommitRange() then maps each previously uncommitted run by:

1. allocating page-aligned backing storage
2. finding a code-memory alias with virtmemFindCodeMemory()
3. mapping the backing storage with svcMapProcessCodeMemory()
4. giving the alias RW permission
5. mapping the alias into the guest window with svcMapProcessMemory()
6. retaining the backing and alias handles for the lifetime of the process

The page allocator now calls this backend before marking Xbox pages committed.

## What is intentionally not solved yet

The first backend does not yet implement physical unmapping on Decommit()/Release(), and it does not yet size the runtime heap around the Switch application's physical-memory budget.

That is deliberate. We want the first hardware probe to establish that the virtual reservation, sparse mapping and guest addressing primitive are sound before tuning the allocator or trying to boot the game.

## First hardware experiment

The Switch bootstrap executable now:

1. starts under libnx
2. reserves the 4 GB guest window
3. commits a single page outside the initial bootstrap range
4. writes 0x5241594D (RAYM) through a guest pointer
5. reads it back
6. waits for the user to return to HOME

This probe has been validated on hardware: the window was reserved and preserved, the dynamic page commit succeeded, and the guest-addressed value round-tripped correctly. That validates the first real Horizon memory primitive; it is not evidence that the full game runtime is ready.

## Important constraint

Do not reserve the entire expected guest range blindly and then discover that the application's heap has been starved.

The current implementation intentionally keeps the 4 GB address window sparse. The final heap split should be derived from actual Rayman startup requirements and measured on hardware.

## Next memory work

After the bootstrap probe works, the next memory tasks are:

- add safe cleanup/unmapping
- commit exactly the regions required by the XEX loader and runtime heap
- audit fixed-address allocations
- audit executable/code mappings
- measure physical-memory use on real hardware
