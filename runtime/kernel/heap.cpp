#include "heap.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <o1heap.h>
#include "memory.h"

RuntimeHeap g_runtimeHeap;

// Cabeçalho próprio com o tamanho pedido (o o1heap não expõe o tamanho do bloco).
// 16 bytes para manter o alinhamento de 16 que o guest espera.
constexpr size_t HEADER_SIZE = 16;

namespace
{
#if defined(__SWITCH__)
constexpr size_t SWITCH_INITIAL_COMMIT = 1 * 1024 * 1024;
constexpr size_t SWITCH_COMMIT_GRANULARITY = 16 * 1024 * 1024;

size_t AlignUp(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

size_t RoundFragmentSize(size_t size)
{
    size = std::max<size_t>(1, size) + O1HEAP_ALIGNMENT;
    size_t rounded = 1;
    while (rounded < size)
        rounded <<= 1;
    return rounded;
}
#endif
}

bool RuntimeHeap::Init(uint32_t guestBase, uint32_t size)
{
    std::lock_guard lock(mutex_);
    if (heap_ != nullptr)
        return true;

    base_ = static_cast<uint8_t*>(g_memory.Translate(guestBase));
    size_ = size;

#if defined(__SWITCH__)
    committedPrefix_ = 0;
    touchedPrefix_ = 0;
    const size_t initialCommit = std::min(size_, SWITCH_INITIAL_COMMIT);
    if (!g_memory.CommitRange(guestBase, initialCommit))
        return false;
    committedPrefix_ = initialCommit;
#endif

    heap_ = o1heapInit(base_, size_);
    return heap_ != nullptr;
}

bool RuntimeHeap::IsInitialized() const
{
    std::lock_guard lock(mutex_);
    return heap_ != nullptr;
}

void* RuntimeHeap::Alloc(size_t size)
{
    uint8_t* block;
    {
        std::lock_guard lock(mutex_);
        if (heap_ == nullptr)
            return nullptr;

#if defined(__SWITCH__)
        const size_t fragmentSize = RoundFragmentSize(size + HEADER_SIZE);
        const size_t neededPrefix = touchedPrefix_ + fragmentSize;
        if (neededPrefix > size_)
            return nullptr;

        if (neededPrefix > committedPrefix_)
        {
            const size_t requestedPrefix = std::min(
                size_, AlignUp(neededPrefix, SWITCH_COMMIT_GRANULARITY));
            if (!g_memory.CommitRange(
                    static_cast<uint32_t>(base_ - g_memory.base) + committedPrefix_,
                    requestedPrefix - committedPrefix_))
            {
                return nullptr;
            }
            committedPrefix_ = requestedPrefix;
        }
#endif

        block = static_cast<uint8_t*>(o1heapAllocate(heap_, size + HEADER_SIZE));

#if defined(__SWITCH__)
        if (block != nullptr)
        {
            const size_t blockOffset = static_cast<size_t>(block - base_);
            const size_t fragmentStart = blockOffset - O1HEAP_ALIGNMENT;
            touchedPrefix_ = std::max(touchedPrefix_, fragmentStart + fragmentSize);
        }
#endif
    }
    if (block == nullptr)
        return nullptr;
    *reinterpret_cast<size_t*>(block) = size;
    return block + HEADER_SIZE;
}

void* RuntimeHeap::AllocZeroed(size_t size)
{
    void* ptr = Alloc(size);
    if (ptr != nullptr)
        memset(ptr, 0, size);
    return ptr;
}

void RuntimeHeap::Free(void* ptr)
{
    if (ptr == nullptr)
        return;
    std::lock_guard lock(mutex_);
    o1heapFree(heap_, static_cast<uint8_t*>(ptr) - HEADER_SIZE);
}

size_t RuntimeHeap::Size(const void* ptr) const
{
    return ptr ? *reinterpret_cast<const size_t*>(static_cast<const uint8_t*>(ptr) - HEADER_SIZE) : 0;
}
