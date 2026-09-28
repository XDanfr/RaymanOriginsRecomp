#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <utility>

struct O1HeapInstance;

// Heap interno do runtime, dentro do espaço do guest (0x90000000..0x9FFFFFFF):
// blocos de thread, objetos do kernel, XamAlloc. Usa o o1heap.
class RuntimeHeap
{
public:
    // On Switch the guest arena is reserved but not all physically backed.
    // Init commits a small prefix; Alloc grows that prefix as needed.
    bool Init(uint32_t guestBase, uint32_t size);
    bool IsInitialized() const;

    void* Alloc(size_t size);            // alinhado a 16 bytes
    void* AllocZeroed(size_t size);
    void Free(void* ptr);
    size_t Size(const void* ptr) const;

    template<typename T, typename... Args>
    T* New(Args&&... args)
    {
        return new (Alloc(sizeof(T))) T(std::forward<Args>(args)...);
    }

private:
    O1HeapInstance* heap_ = nullptr;
    uint8_t* base_ = nullptr;
    size_t size_ = 0;
#if defined(__SWITCH__)
    size_t committedPrefix_ = 0;
    size_t touchedPrefix_ = 0;
#endif
    mutable std::mutex mutex_;
};

extern RuntimeHeap g_runtimeHeap;
