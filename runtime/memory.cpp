#include "memory.h"
#include <cstdio>
#include <limits>

#if defined(__SWITCH__)
#include <cstring>
#include <malloc.h>
#include <switch.h>
#else
#include <sys/mman.h>
#endif

GuestMemory g_memory;

extern "C" void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}

#if defined(__SWITCH__)
namespace
{
constexpr size_t SWITCH_PAGE_SIZE = 0x1000;
constexpr size_t SWITCH_LOW_MEMORY_COMMIT_END = 0x20000;
constexpr unsigned SVC_MAP_PROCESS_MEMORY = 0x74;
constexpr unsigned SVC_MAP_PROCESS_CODE_MEMORY = 0x77;

constexpr size_t AlignUp(size_t value, size_t alignment) noexcept
{
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr size_t AlignDown(size_t value, size_t alignment) noexcept
{
    return value & ~(alignment - 1);
}

bool HasSwitchProcessMemorySyscalls() noexcept
{
    return envIsSyscallHinted(SVC_MAP_PROCESS_MEMORY) &&
           envIsSyscallHinted(0x75) &&
           envIsSyscallHinted(SVC_MAP_PROCESS_CODE_MEMORY) &&
           envIsSyscallHinted(0x78);
}

bool MapSwitchProcessMemoryRange(GuestMemory& memory, size_t offset, size_t size) noexcept
{
    if (size == 0)
        return true;

    const size_t alignedOffset = AlignDown(offset, SWITCH_PAGE_SIZE);
    const size_t alignedEnd = AlignUp(offset + size, SWITCH_PAGE_SIZE);
    const size_t alignedSize = alignedEnd - alignedOffset;
    const uintptr_t destination =
        reinterpret_cast<uintptr_t>(memory.base) + alignedOffset;

    void* backing = memalign(SWITCH_PAGE_SIZE, alignedSize);
    if (backing == nullptr)
    {
        memory.switchInitFailureReason = "Switch backing allocation failed";
        return false;
    }

    std::memset(backing, 0, alignedSize);

    void* codeAlias = nullptr;
    bool codeAliasMapped = false;
    Result rc = 0;

    virtmemLock();
    codeAlias = virtmemFindCodeMemory(alignedSize, SWITCH_PAGE_SIZE);
    if (codeAlias != nullptr)
    {
        rc = svcMapProcessCodeMemory(
            envGetOwnProcessHandle(),
            reinterpret_cast<uintptr_t>(codeAlias),
            reinterpret_cast<uintptr_t>(backing),
            alignedSize
        );

        if (R_SUCCEEDED(rc))
        {
            codeAliasMapped = true;
            rc = svcSetProcessMemoryPermission(
                envGetOwnProcessHandle(),
                reinterpret_cast<uintptr_t>(codeAlias),
                alignedSize,
                Perm_Rw
            );
        }
    }
    virtmemUnlock();

    if (codeAlias == nullptr)
    {
        free(backing);
        memory.switchInitFailureReason = "virtmemFindCodeMemory failed";
        return false;
    }

    if (R_FAILED(rc))
    {
        if (codeAliasMapped)
        {
            svcUnmapProcessCodeMemory(
                envGetOwnProcessHandle(),
                reinterpret_cast<uintptr_t>(codeAlias),
                reinterpret_cast<uintptr_t>(backing),
                alignedSize
            );
        }

        free(backing);
        memory.switchInitResult = static_cast<uint32_t>(rc);
        memory.switchInitFailureReason = "code memory alias setup failed";
        return false;
    }

    rc = svcMapProcessMemory(
        reinterpret_cast<void*>(destination),
        envGetOwnProcessHandle(),
        reinterpret_cast<uintptr_t>(codeAlias),
        alignedSize
    );

    if (R_FAILED(rc))
    {
        svcUnmapProcessCodeMemory(
            envGetOwnProcessHandle(),
            reinterpret_cast<uintptr_t>(codeAlias),
            reinterpret_cast<uintptr_t>(backing),
            alignedSize
        );
        free(backing);
        memory.switchInitResult = static_cast<uint32_t>(rc);
        memory.switchInitFailureReason = "svcMapProcessMemory failed";
        return false;
    }

    try
    {
        memory.switchCommitChunks.push_back({
            alignedOffset,
            alignedSize,
            backing,
            codeAlias
        });
    }
    catch (...)
    {
        svcUnmapProcessMemory(
            reinterpret_cast<void*>(destination),
            envGetOwnProcessHandle(),
            reinterpret_cast<uintptr_t>(codeAlias),
            alignedSize
        );
        svcUnmapProcessCodeMemory(
            envGetOwnProcessHandle(),
            reinterpret_cast<uintptr_t>(codeAlias),
            reinterpret_cast<uintptr_t>(backing),
            alignedSize
        );
        free(backing);
        memory.switchInitFailureReason = "commit chunk tracking allocation failed";
        return false;
    }

    return true;
}
}
#endif

bool GuestMemory::Init()
{
    if (base != nullptr)
        return true;

#if defined(__SWITCH__)
    if (!HasSwitchProcessMemorySyscalls())
    {
        switchInitFailureReason = "process memory syscalls are not hinted";
        return false;
    }

    virtmemLock();
    base = static_cast<uint8_t*>(virtmemFindAslr(PPC_MEMORY_SIZE, SWITCH_PAGE_SIZE));
    if (base != nullptr)
        reservation = virtmemAddReservation(base, PPC_MEMORY_SIZE);
    virtmemUnlock();

    if (base == nullptr)
    {
        switchInitFailureReason = "virtmemFindAslr 4GB window failed";
        return false;
    }

    switchSelectedBase = reinterpret_cast<uintptr_t>(base);

    if (reservation == nullptr)
    {
        switchInitFailureReason = "virtmemAddReservation failed";
        base = nullptr;
        return false;
    }

    try
    {
        committedPages.assign(PPC_MEMORY_SIZE / SWITCH_PAGE_SIZE, 0);
    }
    catch (...)
    {
        switchInitFailureReason = "committed page table allocation failed";
        base = nullptr;
        reservation = nullptr;
        return false;
    }

    if (!CommitRange(
            SWITCH_PAGE_SIZE,
            SWITCH_LOW_MEMORY_COMMIT_END - SWITCH_PAGE_SIZE))
    {
        switchInitFailureReason = "initial low memory commit failed";
        return false;
    }

    const size_t lookupTableSize = static_cast<size_t>(PPC_CODE_SIZE) * 2;
    const size_t imageAndLookupSize =
        static_cast<size_t>(PPC_IMAGE_SIZE) + lookupTableSize;

    if (!CommitRange(PPC_IMAGE_BASE, imageAndLookupSize))
    {
        switchInitFailureReason = "initial image/lookup commit failed";
        return false;
    }
#else
    base = reinterpret_cast<uint8_t*>(mmap(
        reinterpret_cast<void*>(0x100000000ull),
        PPC_MEMORY_SIZE,
        PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_PRIVATE,
        -1,
        0
    ));

    if (base == reinterpret_cast<uint8_t*>(MAP_FAILED))
    {
        base = reinterpret_cast<uint8_t*>(mmap(
            nullptr,
            PPC_MEMORY_SIZE,
            PROT_READ | PROT_WRITE,
            MAP_ANON | MAP_PRIVATE,
            -1,
            0
        ));
    }

    if (base == reinterpret_cast<uint8_t*>(MAP_FAILED))
    {
        base = nullptr;
        fprintf(stderr, "[memory] mmap de 4 GB falhou\n");
        return false;
    }

    mprotect(base, 4096, PROT_NONE);
#endif

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(uint32_t(PPCFuncMappings[i].guest), PPCFuncMappings[i].host);
    }

#if defined(__SWITCH__)
    switchInitFailureReason = nullptr;
    printf("[memory] guest base = %p\n", static_cast<void*>(base));
#else
    fprintf(stderr, "[memory] guest base = %p\n", static_cast<void*>(base));
#endif

    return true;
}

bool GuestMemory::CommitRange(uint32_t guest, size_t size)
{
    if (size == 0)
        return base != nullptr;

    const size_t offset = guest;
    if (base == nullptr || offset >= PPC_MEMORY_SIZE ||
        size > PPC_MEMORY_SIZE - offset)
        return false;

#if defined(__SWITCH__)
    const size_t begin = AlignDown(offset, SWITCH_PAGE_SIZE);
    const size_t end = AlignUp(offset + size, SWITCH_PAGE_SIZE);

    std::lock_guard lock(commitMutex);

    size_t pageOffset = begin;
    while (pageOffset < end)
    {
        const size_t pageIndex = pageOffset / SWITCH_PAGE_SIZE;

        if (committedPages[pageIndex] != 0)
        {
            pageOffset += SWITCH_PAGE_SIZE;
            continue;
        }

        const size_t runStart = pageOffset;
        do
        {
            pageOffset += SWITCH_PAGE_SIZE;
        }
        while (
            pageOffset < end &&
            committedPages[pageOffset / SWITCH_PAGE_SIZE] == 0
        );

        if (!MapSwitchProcessMemoryRange(*this, runStart, pageOffset - runStart))
            return false;

        for (
            size_t committedOffset = runStart;
            committedOffset < pageOffset;
            committedOffset += SWITCH_PAGE_SIZE
        )
        {
            committedPages[committedOffset / SWITCH_PAGE_SIZE] = 1;
        }
    }

    return true;
#else
    return true;
#endif
}
