#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <pthread.h>
#include "objects.h"

struct PPCContext;

struct GuestThreadExit
{
    uint32_t exitCode;
};

// Thread do guest: um pthread do host com o seu próprio bloco PCR/TLS/TEB/pilha
// na memória do guest e o seu próprio PPCContext.
struct GuestThread final : KernelObject
{
    uint32_t id = 0;
    uint32_t handle = 0;
    uint32_t block = 0;           // endereço do PCR (r13)
    uint32_t stackSize = 0;
    int32_t priority = 0;
    uint32_t affinity = 0;
    uint32_t exitCode = 0;
    bool exited = false;          // protegido pela trava do dispatcher
    int32_t suspendCount = 0;     // idem

    uint32_t startAddress = 0, startContext = 0, xapiStartup = 0;
    pthread_t host{};

    bool IsSignaled() const override { return exited; }
};

// Cria o bloco da thread principal e prepara ctx. Chamar uma vez, no main.
std::shared_ptr<GuestThread> InitMainThread(PPCContext& ctx);

std::shared_ptr<GuestThread> GetCurrentGuestThread();

// Encerra a thread atual do guest (ExTerminateThread): desenrola até a base da thread.
[[noreturn]] void ExitCurrentThread(uint32_t exitCode);
