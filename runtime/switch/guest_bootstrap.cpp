#include "guest_bootstrap.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <pthread.h>

#include "cpu/guest_context.h"
#include "kernel/thread.h"
#include "memory.h"

namespace
{
constexpr size_t GUEST_BOOTSTRAP_HOST_STACK = 1 * 1024 * 1024;

std::atomic<GuestExecutionState> g_state{ GuestExecutionState::NotStarted };
std::atomic<uint32_t> g_executionResult{ 0 };
std::mutex g_startMutex;
std::condition_variable g_startCondition;
GuestBootstrapResult g_bootstrapResult;
uint32_t g_entryPoint = 0;
bool g_runRequested = false;

void* GuestBootstrapMain(void* argument)
{
    (void)argument;
    PPCContext context;
    const auto guestThread = InitMainThread(context);
    if (guestThread == nullptr)
    {
        g_state.store(GuestExecutionState::FailedToPrepare, std::memory_order_release);
        g_startCondition.notify_all();
        return nullptr;
    }

    {
        std::lock_guard lock(g_startMutex);
        g_bootstrapResult.pcr = context.r13.u32;
        g_bootstrapResult.guestStackTop = context.r1.u32;
        if (g_bootstrapResult.pcr != guestThread->block ||
            g_bootstrapResult.guestStackTop <= g_bootstrapResult.pcr)
        {
            g_state.store(GuestExecutionState::FailedToPrepare, std::memory_order_release);
            g_startCondition.notify_all();
            ClearPPCContext();
            return nullptr;
        }

        g_state.store(GuestExecutionState::Prepared, std::memory_order_release);
        g_startCondition.notify_all();
    }

    {
        std::unique_lock lock(g_startMutex);
        g_startCondition.wait(lock, [] { return g_runRequested; });
    }

    g_state.store(GuestExecutionState::Running, std::memory_order_release);
    try
    {
        g_memory.FindFunction(g_entryPoint)(context, g_memory.base);
        g_executionResult.store(context.r3.u32, std::memory_order_release);
        g_state.store(GuestExecutionState::Returned, std::memory_order_release);
    }
    catch (const GuestThreadExit& exit)
    {
        g_executionResult.store(exit.exitCode, std::memory_order_release);
        g_state.store(GuestExecutionState::RequestedExit, std::memory_order_release);
    }
    catch (...)
    {
        g_state.store(GuestExecutionState::StoppedByException, std::memory_order_release);
    }

    ClearPPCContext();
    return nullptr;
}
}

bool PrepareGuestEntry(uint32_t entryPoint, GuestBootstrapResult& result)
{
    result = {};
    GuestExecutionState expected = GuestExecutionState::NotStarted;
    if (!g_state.compare_exchange_strong(
            expected, GuestExecutionState::Preparing,
            std::memory_order_acq_rel))
    {
        return false;
    }

    g_entryPoint = entryPoint;
    g_runRequested = false;

    pthread_attr_t attributes;
    const int attrResult = pthread_attr_init(&attributes);
    const int stackResult = attrResult == 0
        ? pthread_attr_setstacksize(&attributes, GUEST_BOOTSTRAP_HOST_STACK)
        : attrResult;

    pthread_t thread;
    const int createResult = stackResult == 0
        ? pthread_create(&thread, &attributes, GuestBootstrapMain, nullptr)
        : stackResult;
    if (attrResult == 0)
        pthread_attr_destroy(&attributes);

    if (createResult != 0)
    {
        fprintf(stderr, "[switch] guest bootstrap pthread creation failed (%d)\n", createResult);
        g_state.store(GuestExecutionState::FailedToPrepare, std::memory_order_release);
        return false;
    }

    pthread_detach(thread);

    std::unique_lock lock(g_startMutex);
    g_startCondition.wait(lock, [] {
        const auto state = g_state.load(std::memory_order_acquire);
        return state == GuestExecutionState::Prepared ||
               state == GuestExecutionState::FailedToPrepare;
    });

    if (g_state.load(std::memory_order_acquire) != GuestExecutionState::Prepared)
    {
        fprintf(stderr, "[switch] guest bootstrap context setup failed\n");
        return false;
    }

    result = g_bootstrapResult;
    return true;
}

void RunGuestEntry()
{
    std::lock_guard lock(g_startMutex);
    if (g_state.load(std::memory_order_acquire) != GuestExecutionState::Prepared)
        return;
    g_runRequested = true;
    g_startCondition.notify_all();
}

GuestExecutionState GetGuestExecutionState()
{
    return g_state.load(std::memory_order_acquire);
}

uint32_t GetGuestExecutionResult()
{
    return g_executionResult.load(std::memory_order_acquire);
}
