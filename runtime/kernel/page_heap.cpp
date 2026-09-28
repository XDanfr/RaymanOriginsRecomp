#include "page_heap.h"
#include <algorithm>
#include <cstring>
#include "memory.h"

PageHeap::PageHeap(uint32_t base, uint32_t size, uint32_t pageSize, uint32_t allocAlign)
    : base_(base), size_(size), pageSize_(pageSize), allocAlign_(std::max(allocAlign, pageSize))
{
    uint32_t pages = size / pageSize;
    state_.assign(pages, Free);
    allocBase_.assign(pages, 0);
    protect_.assign(pages, 0);
    allocPages_.assign(pages, 0);
}

bool PageHeap::RangeIs(uint32_t first, uint32_t count, State state) const
{
    for (uint32_t i = first; i < first + count; i++)
        if (state_[i] != state)
            return false;
    return true;
}

bool PageHeap::Commit(uint32_t first, uint32_t count, uint32_t protect)
{
    const size_t bytes = size_t(count) * pageSize_;
    if (!g_memory.CommitRange(PageAddress(first), bytes))
        return false;

    for (uint32_t i = first; i < first + count; i++)
    {
        if (state_[i] != Committed)
            memset(g_memory.Translate(PageAddress(i)), 0, pageSize_);
        state_[i] = Committed;
        protect_[i] = protect;
    }

    return true;
}

uint32_t PageHeap::Alloc(uint32_t size, uint32_t alignment, bool commit, bool topDown, uint32_t protect)
{
    if (size == 0)
        return 0;

    std::lock_guard lock(mutex_);

    const uint32_t total = uint32_t(state_.size());
    const uint32_t count = (size + pageSize_ - 1) / pageSize_;
    const uint32_t alignPages = std::max(alignment, allocAlign_) / pageSize_;
    auto alignUp = [&](uint32_t i) { return (i + alignPages - 1) / alignPages * alignPages; };
    if (count > total)
        return 0;

    auto firstUsed = [&](uint32_t i) -> uint32_t {
        for (uint32_t j = i; j < i + count; j++)
            if (state_[j] != Free)
                return j;
        return UINT32_MAX;
    };

    uint32_t found = UINT32_MAX;
    if (topDown)
    {
        uint32_t i = (total - count) / alignPages * alignPages;
        while (true)
        {
            uint32_t used = firstUsed(i);
            if (used == UINT32_MAX) { found = i; break; }
            if (i < alignPages) break;
            i -= alignPages;
        }
    }
    else
    {
        for (uint32_t start : { hint_, 0u })
        {
            for (uint32_t i = alignUp(start); i + count <= total;)
            {
                uint32_t used = firstUsed(i);
                if (used == UINT32_MAX) { found = i; break; }
                i = alignUp(used + 1);
            }
            if (found != UINT32_MAX || start == 0)
                break;
        }
    }

    if (found == UINT32_MAX)
        return 0;

    const uint32_t address = PageAddress(found);

    auto rollback = [&]()
    {
        for (uint32_t i = found; i < found + count; i++)
        {
            state_[i] = Free;
            allocBase_[i] = 0;
            protect_[i] = 0;
        }
        allocPages_[found] = 0;
    };

    for (uint32_t i = found; i < found + count; i++)
    {
        state_[i] = Reserved;
        allocBase_[i] = address;
        protect_[i] = protect;
    }
    allocPages_[found] = count;

    if (commit && !Commit(found, count, protect))
    {
        rollback();
        return 0;
    }

    if (!topDown)
        hint_ = found + count;

    return address;
}

bool PageHeap::AllocFixed(
    uint32_t address,
    uint32_t size,
    bool reserve,
    bool commit,
    uint32_t protect,
    bool* wasCommitted
)
{
    if (!Contains(address) || size == 0)
        return false;

    std::lock_guard lock(mutex_);

    const uint32_t first = PageIndex(address);
    const uint32_t count = (address - PageAddress(first) + size + pageSize_ - 1) / pageSize_;
    if (first + count > state_.size())
        return false;

    if (wasCommitted)
        *wasCommitted = RangeIs(first, count, Committed);

    bool newReservation = false;

    if (RangeIs(first, count, Free))
    {
        if (!reserve)
            return false;

        const uint32_t regionBase = PageAddress(first);
        for (uint32_t i = first; i < first + count; i++)
        {
            state_[i] = Reserved;
            allocBase_[i] = regionBase;
            protect_[i] = protect;
        }
        allocPages_[first] = count;
        newReservation = true;
    }
    else
    {
        for (uint32_t i = first; i < first + count; i++)
            if (state_[i] == Free)
                return false;
    }

    if (commit && !Commit(first, count, protect))
    {
        if (newReservation)
        {
            for (uint32_t i = first; i < first + count; i++)
            {
                state_[i] = Free;
                allocBase_[i] = 0;
                protect_[i] = 0;
            }
            allocPages_[first] = 0;
        }
        return false;
    }

    return true;
}

bool PageHeap::Decommit(uint32_t address, uint32_t size)
{
    if (!Contains(address))
        return false;

    std::lock_guard lock(mutex_);
    const uint32_t first = PageIndex(address);
    const uint32_t count = std::min<uint32_t>(
        (size + pageSize_ - 1) / pageSize_,
        uint32_t(state_.size()) - first
    );

    for (uint32_t i = first; i < first + count; i++)
        if (state_[i] == Committed)
            state_[i] = Reserved;

    return true;
}

bool PageHeap::Release(uint32_t address, uint32_t* releasedSize)
{
    if (!Contains(address))
        return false;

    std::lock_guard lock(mutex_);
    const uint32_t first = PageIndex(address);
    const uint32_t count = allocPages_[first];
    if (count == 0 || allocBase_[first] != PageAddress(first))
        return false;

    for (uint32_t i = first; i < first + count; i++)
    {
        state_[i] = Free;
        allocBase_[i] = 0;
        protect_[i] = 0;
    }
    allocPages_[first] = 0;
    if (releasedSize)
        *releasedSize = count * pageSize_;

    return true;
}

bool PageHeap::Query(uint32_t address, RegionInfo& out) const
{
    if (!Contains(address))
        return false;

    std::lock_guard lock(mutex_);
    const uint32_t first = PageIndex(address);
    uint32_t last = first;
    while (
        last + 1 < state_.size() &&
        state_[last + 1] == state_[first] &&
        allocBase_[last + 1] == allocBase_[first] &&
        protect_[last + 1] == protect_[first]
    )
    {
        last++;
    }

    out.baseAddress = PageAddress(first);
    out.allocationBase = allocBase_[first];
    out.allocationProtect = out.allocationBase ? protect_[PageIndex(out.allocationBase)] : 0;
    out.regionSize = (last - first + 1) * pageSize_;
    out.state = state_[first];
    out.protect = protect_[first];
    return true;
}

uint32_t PageHeap::AllocationSize(uint32_t address) const
{
    if (!Contains(address))
        return 0;

    std::lock_guard lock(mutex_);
    const uint32_t first = PageIndex(address);
    return allocBase_[first] == address ? allocPages_[first] * pageSize_ : 0;
}
