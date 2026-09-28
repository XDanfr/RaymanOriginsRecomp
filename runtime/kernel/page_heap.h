#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

// Alocador de páginas para uma faixa do espaço do guest, com a semântica de
// reserva/commit do kernel do Xbox 360 (NtAllocateVirtualMemory e afins).
// O backing real é fornecido por GuestMemory, inclusive no Switch.
class PageHeap
{
public:
    enum State : uint8_t { Free = 0, Reserved = 1, Committed = 2 };

    struct RegionInfo
    {
        uint32_t baseAddress;
        uint32_t allocationBase;
        uint32_t allocationProtect;
        uint32_t regionSize;
        State state;
        uint32_t protect;
    };

    PageHeap(uint32_t base, uint32_t size, uint32_t pageSize, uint32_t allocAlign);

    bool Contains(uint32_t address) const { return address >= base_ && address - base_ < size_; }
    uint32_t PageSize() const { return pageSize_; }

    uint32_t Alloc(uint32_t size, uint32_t alignment, bool commit, bool topDown, uint32_t protect);

    bool AllocFixed(
        uint32_t address,
        uint32_t size,
        bool reserve,
        bool commit,
        uint32_t protect,
        bool* wasCommitted
    );

    bool Decommit(uint32_t address, uint32_t size);
    bool Release(uint32_t address, uint32_t* releasedSize);

    bool Query(uint32_t address, RegionInfo& out) const;
    uint32_t AllocationSize(uint32_t address) const;

private:
    uint32_t PageIndex(uint32_t address) const { return (address - base_) / pageSize_; }
    uint32_t PageAddress(uint32_t index) const { return base_ + index * pageSize_; }
    bool RangeIs(uint32_t first, uint32_t count, State state) const;
    bool Commit(uint32_t first, uint32_t count, uint32_t protect);

    const uint32_t base_, size_, pageSize_, allocAlign_;
    std::vector<State> state_;
    std::vector<uint32_t> allocBase_;
    std::vector<uint32_t> protect_;
    std::vector<uint32_t> allocPages_;
    uint32_t hint_ = 0;
    mutable std::mutex mutex_;
};
