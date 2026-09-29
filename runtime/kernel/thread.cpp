// Threads e TLS do guest. Layout do bloco da thread igual ao do Unleashed
// Recompiled (cpu/guest_thread.cpp); ExCreateThread segue o Xenia (chama o
// XapiThreadStartup do jogo com a rotina e o contexto).
#include "thread.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "cpu/guest_context.h"
#include "function.h"
#include "heap.h"
#include "memory.h"
#include "xbox_defs.h"

constexpr uint32_t PCR_SIZE = 0xAB0;
constexpr uint32_t TLS_SIZE = 0x100;
constexpr uint32_t TEB_SIZE = 0x2E0;
constexpr uint32_t DEFAULT_STACK_SIZE = 0x40000;
#if defined(__SWITCH__)
// libnx has a finite shared stack VA region. The PPC stack itself lives in
// guest memory, so this covers only the translated C++ call frames.
constexpr size_t HOST_STACK_SIZE = 1 * 1024 * 1024;
#else
constexpr size_t HOST_STACK_SIZE = 16 * 1024 * 1024;
#endif

static thread_local std::shared_ptr<GuestThread> t_thread;
static std::atomic<uint32_t> g_nextThreadId{ 1 };

// TEB (o "KTHREAD" em PCR+0x100) -> thread. O jogo pode ler KeGetCurrentThread()
// direto do PCR e passar esse ponteiro para Ke*Thread em vez de um handle.
static std::mutex g_tebMutex;
static std::unordered_map<uint32_t, std::weak_ptr<GuestThread>> g_threadsByTeb;

static std::shared_ptr<GuestThread> ThreadFromPointerOrHandle(uint32_t value)
{
    if (IsHandle(value))
        return GetObjectAs<GuestThread>(value);
    std::lock_guard lock(g_tebMutex);
    auto it = g_threadsByTeb.find(value);
    return it != g_threadsByTeb.end() ? it->second.lock() : nullptr;
}

std::shared_ptr<GuestThread> GetCurrentGuestThread()
{
    return t_thread;
}

std::shared_ptr<KernelObject> GetCurrentThreadObject()
{
    return t_thread;
}

static void Store32(uint32_t guest, uint32_t value)
{
    *static_cast<be<uint32_t>*>(g_memory.Translate(guest)) = value;
}

// Monta PCR/TLS/TEB/pilha e o contexto de CPU da thread.
static bool SetupThreadBlock(GuestThread& thread, PPCContext& ctx, uint32_t cpuNumber)
{
    uint32_t stackSize = thread.stackSize ? (thread.stackSize + 0xFFF) & ~0xFFFu : DEFAULT_STACK_SIZE;
    uint32_t total = PCR_SIZE + TLS_SIZE + TEB_SIZE + stackSize;
    void* block = g_runtimeHeap.AllocZeroed(total);
    if (block == nullptr)
    {
        fprintf(stderr, "[thread] guest thread block allocation failed (0x%X bytes)\n", total);
        return false;
    }
    thread.block = g_memory.MapVirtual(block);

    uint32_t tls = thread.block + PCR_SIZE;
    uint32_t teb = tls + TLS_SIZE;

    Store32(thread.block + 0x0, tls);                                 // PCR: ponteiro de TLS
    Store32(thread.block + 0x100, teb);                               // PCR: ponteiro do KTHREAD/TEB
    *static_cast<uint8_t*>(g_memory.Translate(thread.block + 0x10C)) = uint8_t(cpuNumber);
    Store32(tls + 0x10, 0xFFFFFFFF);                                  // entrada de TLS que o Unleashed também seta
    Store32(teb + 0x14C, thread.id);                                  // id da thread
    {
        std::lock_guard lock(g_tebMutex);
        g_threadsByTeb[teb] = std::static_pointer_cast<GuestThread>(thread.shared_from_this());
    }

    ctx = {};
    ctx.r1.u64 = teb + TEB_SIZE + stackSize;                          // topo da pilha
    ctx.r13.u64 = thread.block;                                       // r13 = PCR
    ctx.fpscr.loadFromHost();
    SetPPCContext(ctx);
    return true;
}

std::shared_ptr<GuestThread> InitMainThread(PPCContext& ctx)
{
    auto thread = std::make_shared<GuestThread>();
    thread->id = g_nextThreadId++;
    thread->handle = CreateHandle(thread);
    thread->host = pthread_self();
    if (!SetupThreadBlock(*thread, ctx, 0))
    {
        CloseHandle(thread->handle);
        return nullptr;
    }
    t_thread = thread;
    return thread;
}

[[noreturn]] void ExitCurrentThread(uint32_t exitCode)
{
    throw GuestThreadExit{ exitCode };
}

static void* GuestThreadMain(void* arg)
{
    // Assume a referência criada em ExCreateThread.
    std::shared_ptr<GuestThread> thread(*static_cast<std::shared_ptr<GuestThread>*>(arg));
    delete static_cast<std::shared_ptr<GuestThread>*>(arg);
    t_thread = thread;

    PPCContext ctx;
    if (!SetupThreadBlock(*thread, ctx, thread->id % 6))
    {
        auto lock = LockDispatcher();
        thread->exitCode = X_STATUS_NO_MEMORY;
        thread->exited = true;
        NotifyDispatcher();
        return nullptr;
    }

    {
        // Criada suspensa: espera o NtResumeThread.
        auto lock = LockDispatcher();
        while (thread->suspendCount > 0)
        {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            lock.lock();
        }
    }

    uint32_t exitCode = 0;
    try
    {
        if (thread->xapiStartup != 0)
        {
            // XapiThreadStartup(rotina, contexto): inicializa o CRT da thread e chama a rotina.
            ctx.r3.u64 = thread->startAddress;
            ctx.r4.u64 = thread->startContext;
            g_memory.FindFunction(thread->xapiStartup)(ctx, g_memory.base);
        }
        else
        {
            ctx.r3.u64 = thread->startContext;
            g_memory.FindFunction(thread->startAddress)(ctx, g_memory.base);
        }
        exitCode = ctx.r3.u32;
    }
    catch (const GuestThreadExit& exit)
    {
        exitCode = exit.exitCode;
    }

    {
        auto lock = LockDispatcher();
        thread->exitCode = exitCode;
        thread->exited = true;
    }
    NotifyDispatcher();
    fprintf(stderr, "[thread] thread %u terminou (código %u)\n", thread->id, exitCode);
    return nullptr;
}

static uint32_t ExCreateThread(be<uint32_t>* handle, uint32_t stackSize, be<uint32_t>* threadId,
                               uint32_t xapiStartup, uint32_t startAddress, uint32_t startContext, uint32_t creationFlags)
{
    auto thread = std::make_shared<GuestThread>();
    thread->id = g_nextThreadId++;
    thread->stackSize = stackSize;
    thread->xapiStartup = xapiStartup;
    thread->startAddress = startAddress;
    thread->startContext = startContext;
    thread->suspendCount = (creationFlags & 0x1) ? 1 : 0; // CREATE_SUSPENDED
    thread->handle = CreateHandle(thread);

    fprintf(stderr, "[thread] ExCreateThread id=%u rotina=0x%08X ctx=0x%08X startup=0x%08X pilha=0x%X flags=0x%X\n",
            thread->id, startAddress, startContext, xapiStartup, stackSize, creationFlags);

    if (g_memory.FindFunction(xapiStartup ? xapiStartup : startAddress) == nullptr)
    {
        fprintf(stderr, "[thread] rotina 0x%08X sem função recompilada\n", xapiStartup ? xapiStartup : startAddress);
        CloseHandle(thread->handle);
        return X_STATUS_INVALID_PARAMETER;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HOST_STACK_SIZE);
    auto* arg = new std::shared_ptr<GuestThread>(thread);
    int err = pthread_create(&thread->host, &attr, GuestThreadMain, arg);
    pthread_attr_destroy(&attr);
    if (err != 0)
    {
        delete arg;
        CloseHandle(thread->handle);
        return X_STATUS_NO_MEMORY;
    }
    pthread_detach(thread->host);

    if (handle)
        handle->set(thread->handle);
    if (threadId)
        threadId->set(thread->id);
    return X_STATUS_SUCCESS;
}

static void ExTerminateThread(uint32_t exitCode)
{
    ExitCurrentThread(exitCode);
}

static uint32_t NtResumeThread(uint32_t handle, be<uint32_t>* previousCount)
{
    auto thread = GetObjectAs<GuestThread>(handle);
    if (!thread)
        return STATUS_INVALID_HANDLE;

    auto lock = LockDispatcher();
    if (previousCount)
        previousCount->set(uint32_t(thread->suspendCount));
    if (thread->suspendCount > 0)
        thread->suspendCount--;
    return X_STATUS_SUCCESS;
}

static uint32_t NtSuspendThread(uint32_t handle, be<uint32_t>* previousCount)
{
    auto thread = GetObjectAs<GuestThread>(handle);
    if (!thread)
        return STATUS_INVALID_HANDLE;

    // Suspender outra thread em ponto arbitrário não é possível sem cooperação;
    // só contabilizamos. Registrar para ver se o jogo depende disso.
    fprintf(stderr, "[thread] NtSuspendThread(%u) ignorado\n", thread->id);
    auto lock = LockDispatcher();
    if (previousCount)
        previousCount->set(uint32_t(thread->suspendCount));
    return X_STATUS_SUCCESS;
}

// Ke*Thread recebem o "objeto" devolvido por ObReferenceObjectByHandle, que é o próprio handle.
static uint32_t KeSetBasePriorityThread(uint32_t thread, int32_t priority)
{
    auto object = ThreadFromPointerOrHandle(thread);
    if (!object)
        return 0;
    int32_t previous = object->priority;
    object->priority = priority;
    return uint32_t(previous);
}

static uint32_t KeQueryBasePriorityThread(uint32_t thread)
{
    auto object = ThreadFromPointerOrHandle(thread);
    return object ? uint32_t(object->priority) : 0;
}

static uint32_t KeSetAffinityThread(uint32_t thread, uint32_t affinity, be<uint32_t>* previous)
{
    auto object = ThreadFromPointerOrHandle(thread);
    if (previous)
        previous->set(object ? (object->affinity ? object->affinity : 0x3F) : 0x3F);
    if (object)
        object->affinity = affinity;
    return X_STATUS_SUCCESS;
}

static uint32_t KeDelayExecutionThread(uint32_t waitMode, uint32_t alertable, be<int64_t>* timeout)
{
    int64_t ms = GuestTimeoutToMs(timeout);
    if (ms <= 0)
        std::this_thread::yield();
    else
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    (void)waitMode; (void)alertable;
    return X_STATUS_SUCCESS;
}

static uint32_t NtYieldExecution()
{
    std::this_thread::yield();
    return X_STATUS_SUCCESS;
}

// Exported by xboxkrnl as a callback placeholder.  The game calls it while
// constructing its APC data, and the kernel implementation intentionally has
// no observable work to perform.
static uint32_t KiApcNormalRoutineNop()
{
    return 0;
}

// ---- TLS (igual ao Unleashed: vetor por thread) ----

static std::mutex g_tlsMutex;
static std::vector<uint32_t> g_tlsFreeIndices;
static uint32_t g_tlsNextIndex = 0;

static uint32_t& TlsSlot(uint32_t index)
{
    thread_local std::vector<uint32_t> values;
    if (values.size() <= index)
        values.resize(index + 1, 0);
    return values[index];
}

static uint32_t KeTlsAlloc()
{
    std::lock_guard lock(g_tlsMutex);
    if (!g_tlsFreeIndices.empty())
    {
        uint32_t index = g_tlsFreeIndices.back();
        g_tlsFreeIndices.pop_back();
        return index;
    }
    return g_tlsNextIndex++;
}

static uint32_t KeTlsFree(uint32_t index)
{
    std::lock_guard lock(g_tlsMutex);
    g_tlsFreeIndices.push_back(index);
    return 1;
}

static uint32_t KeTlsGetValue(uint32_t index)
{
    return TlsSlot(index);
}

static uint32_t KeTlsSetValue(uint32_t index, uint32_t value)
{
    TlsSlot(index) = value;
    return 1;
}

GUEST_FUNCTION_HOOK(__imp__ExCreateThread, ExCreateThread);
GUEST_FUNCTION_HOOK(__imp__ExTerminateThread, ExTerminateThread);
GUEST_FUNCTION_HOOK(__imp__NtResumeThread, NtResumeThread);
GUEST_FUNCTION_HOOK(__imp__NtSuspendThread, NtSuspendThread);
GUEST_FUNCTION_HOOK(__imp__KeSetBasePriorityThread, KeSetBasePriorityThread);
GUEST_FUNCTION_HOOK(__imp__KeQueryBasePriorityThread, KeQueryBasePriorityThread);
GUEST_FUNCTION_HOOK(__imp__KeSetAffinityThread, KeSetAffinityThread);
GUEST_FUNCTION_HOOK(__imp__KeDelayExecutionThread, KeDelayExecutionThread);
GUEST_FUNCTION_HOOK(__imp__NtYieldExecution, NtYieldExecution);
GUEST_FUNCTION_HOOK(__imp__KiApcNormalRoutineNop, KiApcNormalRoutineNop);
GUEST_FUNCTION_HOOK(__imp__KeTlsAlloc, KeTlsAlloc);
GUEST_FUNCTION_HOOK(__imp__KeTlsFree, KeTlsFree);
GUEST_FUNCTION_HOOK(__imp__KeTlsGetValue, KeTlsGetValue);
GUEST_FUNCTION_HOOK(__imp__KeTlsSetValue, KeTlsSetValue);
