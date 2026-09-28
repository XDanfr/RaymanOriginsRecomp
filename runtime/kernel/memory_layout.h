#pragma once
#include <cstdint>

// Mapa do espaço de 32 bits do guest.
//   0x00010000..0x3FFFFFFF  NtAllocateVirtualMemory, páginas de 4 KB
//   0x40000000..0x7EFFFFFF  NtAllocateVirtualMemory, páginas de 64 KB
//   0x82000000..            imagem do XEX + tabela de funções (PPC_LOOKUP_FUNC)
//   0x90000000..0x9FFFFFFF  heap interno do runtime
//   0xA0000000..0xFFFFFFFF  memória física (MmAllocatePhysicalMemoryEx)
constexpr uint32_t RUNTIME_HEAP_BASE = 0x90000000;
constexpr uint32_t RUNTIME_HEAP_SIZE = 0x10000000;

// Cria os heaps do guest. Chamar depois de g_memory.Init().
bool InitGuestHeaps();

// Memória física zerada para estruturas lidas pelo hardware (contextos XMA, etc.).
uint32_t AllocatePhysicalMemory(uint32_t size, uint32_t alignment);
