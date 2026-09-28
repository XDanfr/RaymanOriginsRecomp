// Imports de memória do kernel/XAM. Semântica baseada no Xenia
// (xboxkrnl_memory.cc): o Unleashed não precisa disto porque intercepta o heap
// do próprio Sonic, mas o Rayman usa o NtAllocateVirtualMemory para montar o seu.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "function.h"
#include "heap.h"
#include "memory_layout.h"
#include "page_heap.h"
#include "xbox_defs.h"

// Faixas iguais às do Xenia: páginas de 4 KB, páginas de 64 KB e memória física.
static PageHeap g_heap4k(0x00010000, 0x3FFF0000, 0x1000, 0x10000);
static PageHeap g_heap64k(0x40000000, 0x3F000000, 0x10000, 0x10000);
// 512 MB, como o hardware: a GPU usa endereços físicos (addr & 0x1FFFFFFF), que
// precisam corresponder a um único endereço aqui (físico + 0xA0000000).
static PageHeap g_physical(0xA0000000, 0x20000000, 0x1000, 0x1000);

static PageHeap* LookupVirtualHeap(uint32_t address)
{
    if (g_heap4k.Contains(address)) return &g_heap4k;
    if (g_heap64k.Contains(address)) return &g_heap64k;
    return nullptr;
}

static PageHeap* LookupAnyHeap(uint32_t address)
{
    if (PageHeap* heap = LookupVirtualHeap(address)) return heap;
    if (g_physical.Contains(address)) return &g_physical;
    return nullptr;
}

bool InitGuestHeaps()
{
    return g_runtimeHeap.Init(RUNTIME_HEAP_BASE, RUNTIME_HEAP_SIZE);
}

static uint32_t NtAllocateVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize,
                                        uint32_t allocType, uint32_t protect, uint32_t debugMemory)
{
    if (!baseAddress || !regionSize || regionSize->get() == 0)
        return X_STATUS_INVALID_PARAMETER;
    if (!(allocType & (X_MEM_COMMIT | X_MEM_RESET | X_MEM_RESERVE)))
        return X_STATUS_INVALID_PARAMETER;
    if (allocType & X_MEM_RESET)
        return (allocType & ~X_MEM_RESET) ? X_STATUS_INVALID_PARAMETER : X_STATUS_SUCCESS; // conteúdo descartável: nada a fazer

    const uint32_t requested = baseAddress->get();
    // Alguns jogos passam tamanho negativo (Xenia).
    uint32_t size = int32_t(regionSize->get()) < 0 ? uint32_t(-int32_t(regionSize->get())) : regionSize->get();

    const bool reserve = allocType & X_MEM_RESERVE;
    const bool commit = allocType & X_MEM_COMMIT;
    uint32_t address = 0;
    uint32_t pageSize;
    bool wasCommitted = false;

    if (requested != 0)
    {
        PageHeap* heap = LookupVirtualHeap(requested);
        if (!heap)
            return X_STATUS_INVALID_PARAMETER;
        pageSize = heap->PageSize();
        uint32_t aligned = requested - requested % pageSize;
        size = (size + (requested - aligned) + pageSize - 1) / pageSize * pageSize;
        if (heap->AllocFixed(aligned, size, reserve, commit, protect, &wasCommitted))
            address = aligned;
    }
    else
    {
        PageHeap& heap = (allocType & X_MEM_LARGE_PAGES) ? g_heap64k : g_heap4k;
        pageSize = heap.PageSize();
        size = (size + 0xFFFF) & ~0xFFFFu; // reservas sem endereço fixo: granularidade de 64 KB
        address = heap.Alloc(size, 0, commit, allocType & X_MEM_TOP_DOWN, protect);
    }

    if (address == 0)
        return X_STATUS_NO_MEMORY;

    // O PageHeap zera no commit; X_MEM_NOZERO só otimiza no hardware real.
    (void)wasCommitted;
    (void)debugMemory;
    baseAddress->set(address);
    regionSize->set(size);
    return X_STATUS_SUCCESS;
}

static uint32_t NtFreeVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t freeType, uint32_t debugMemory)
{
    if (!baseAddress || baseAddress->get() == 0)
        return X_STATUS_MEMORY_NOT_ALLOCATED;

    const uint32_t address = baseAddress->get();
    PageHeap* heap = LookupVirtualHeap(address);
    if (!heap)
        return X_STATUS_INVALID_PARAMETER;

    uint32_t size = regionSize ? regionSize->get() : 0;
    bool ok;
    if (freeType == X_MEM_DECOMMIT)
    {
        size = (size + heap->PageSize() - 1) / heap->PageSize() * heap->PageSize();
        ok = heap->Decommit(address, size);
    }
    else
    {
        ok = heap->Release(address, &size);
    }
    if (!ok)
        return X_STATUS_UNSUCCESSFUL;

    baseAddress->set(address);
    if (regionSize)
        regionSize->set(size);
    (void)debugMemory;
    return X_STATUS_SUCCESS;
}

struct X_MEMORY_BASIC_INFORMATION
{
    be<uint32_t> baseAddress;
    be<uint32_t> allocationBase;
    be<uint32_t> allocationProtect;
    be<uint32_t> regionSize;
    be<uint32_t> state;
    be<uint32_t> protect;
    be<uint32_t> type;
};

static uint32_t NtQueryVirtualMemory(uint32_t address, X_MEMORY_BASIC_INFORMATION* info, uint32_t regionType)
{
    PageHeap* heap = LookupAnyHeap(address);
    PageHeap::RegionInfo region;
    if (!info || !heap || !heap->Query(address, region))
        return X_STATUS_INVALID_PARAMETER;

    info->baseAddress = region.baseAddress;
    info->allocationBase = region.allocationBase;
    info->allocationProtect = region.allocationProtect;
    info->regionSize = region.regionSize;
    info->state = region.state == PageHeap::Committed ? X_MEM_COMMIT
                : region.state == PageHeap::Reserved ? X_MEM_RESERVE : X_MEM_FREE;
    info->protect = region.protect;
    info->type = region.state == PageHeap::Free ? 0 : X_MEM_PRIVATE;
    (void)regionType;
    return X_STATUS_SUCCESS;
}

static uint32_t MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size, uint32_t protect,
                                           uint32_t minAddress, uint32_t maxAddress, uint32_t alignment)
{
    // Tamanho de página vem da proteção (Xenia): 16 MB, 64 KB ou 4 KB.
    uint32_t pageSize = (protect & X_MEM_16MB_PAGES) ? 0x1000000 : (protect & X_MEM_LARGE_PAGES) ? 0x10000 : 0x1000;
    alignment = std::max(alignment, pageSize);
    size = (size + pageSize - 1) / pageSize * pageSize;

    uint32_t address = g_physical.Alloc(size, alignment, true, false, protect);
    if (address == 0)
        fprintf(stderr, "[memory] MmAllocatePhysicalMemoryEx sem memória (size=0x%X align=0x%X)\n", size, alignment);
    (void)flags; (void)minAddress; (void)maxAddress;
    return address;
}

static void MmFreePhysicalMemory(uint32_t type, uint32_t address)
{
    if (address != 0)
        g_physical.Release(address, nullptr);
    (void)type;
}

static uint32_t MmQueryAllocationSize(uint32_t address)
{
    PageHeap* heap = LookupAnyHeap(address);
    return heap ? heap->AllocationSize(address) : 0;
}

static uint32_t MmQueryAddressProtect(uint32_t address)
{
    PageHeap* heap = LookupAnyHeap(address);
    PageHeap::RegionInfo region;
    if (heap && heap->Query(address, region) && region.state == PageHeap::Committed)
        return region.protect;
    return X_PAGE_READWRITE; // imagem do XEX e heap do runtime
}

// Endereço identidade, como no Unleashed: o backend de GPU usa o mesmo espaço.
static uint32_t MmGetPhysicalAddress(uint32_t address)
{
    return address;
}

static uint32_t XamAlloc(uint32_t flags, uint32_t size, be<uint32_t>* outPtr)
{
    void* ptr = g_runtimeHeap.AllocZeroed(size);
    if (!ptr)
        return X_STATUS_NO_MEMORY;
    if (outPtr)
        outPtr->set(g_memory.MapVirtual(ptr));
    (void)flags;
    return X_STATUS_SUCCESS;
}

static uint32_t XamFree(uint32_t address)
{
    if (address != 0)
        g_runtimeHeap.Free(g_memory.Translate(address));
    return X_STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__NtAllocateVirtualMemory, NtAllocateVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__NtFreeVirtualMemory, NtFreeVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__NtQueryVirtualMemory, NtQueryVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__MmAllocatePhysicalMemoryEx, MmAllocatePhysicalMemoryEx);
GUEST_FUNCTION_HOOK(__imp__MmFreePhysicalMemory, MmFreePhysicalMemory);
GUEST_FUNCTION_HOOK(__imp__MmQueryAllocationSize, MmQueryAllocationSize);
GUEST_FUNCTION_HOOK(__imp__MmQueryAddressProtect, MmQueryAddressProtect);
GUEST_FUNCTION_HOOK(__imp__MmGetPhysicalAddress, MmGetPhysicalAddress);
GUEST_FUNCTION_HOOK(__imp__XamAlloc, XamAlloc);
GUEST_FUNCTION_HOOK(__imp__XamFree, XamFree);

// Para o runtime (contextos XMA etc.): memória física zerada e alinhada.
uint32_t AllocatePhysicalMemory(uint32_t size, uint32_t alignment)
{
    return g_physical.Alloc(size, std::max<uint32_t>(alignment, 0x1000), true, false, X_PAGE_READWRITE);
}

// Mapeamento de I/O (usado pelo áudio): na memória plana, o endereço físico já é acessível.
static uint32_t MmMapIoSpace(uint32_t unknown, uint32_t address, uint32_t size, uint32_t flags)
{
    (void)unknown; (void)size; (void)flags;
    return address;
}

GUEST_FUNCTION_HOOK(__imp__MmMapIoSpace, MmMapIoSpace);
