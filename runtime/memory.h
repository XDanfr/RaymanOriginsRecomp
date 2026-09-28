#pragma once
#include "ppc_recomp_shared.h"
#include <cstddef>
#include <cstdint>

#if defined(__SWITCH__)
#include <mutex>
#include <vector>
#endif

// Espaço de endereçamento de 32 bits do Xbox 360 (4 GB), reservado de uma vez.
// Endereço do guest = deslocamento a partir de base.
struct GuestMemory
{
    uint8_t* base = nullptr;

    bool Init();

    // Garante que [guest, guest + size) tem backing físico acessível.
    // No desktop o mmap já fornece memória commitada, então isso só valida o range.
    // No Switch isto cria os aliases Horizon necessários para o range.
    bool CommitRange(uint32_t guest, size_t size);

    void* Translate(uint32_t guest) const { return base + guest; }
    uint32_t MapVirtual(const void* host) const
    {
        return uint32_t(static_cast<const uint8_t*>(host) - base);
    }

    PPCFunc* FindFunction(uint32_t guest) const { return PPC_LOOKUP_FUNC(base, guest); }
    void InsertFunction(uint32_t guest, PPCFunc* host) { PPC_LOOKUP_FUNC(base, guest) = host; }

#if defined(__SWITCH__)
    struct SwitchCommitChunk
    {
        size_t offset = 0;
        size_t size = 0;
        void* backing = nullptr;
        void* codeAlias = nullptr;
    };

    void* reservation = nullptr;
    std::vector<uint8_t> committedPages;
    std::vector<SwitchCommitChunk> switchCommitChunks;
    mutable std::mutex commitMutex;

    uintptr_t switchSelectedBase = 0;
    uint32_t switchInitResult = 0;
    const char* switchInitFailureReason = "not started";
#endif
};

extern GuestMemory g_memory;
