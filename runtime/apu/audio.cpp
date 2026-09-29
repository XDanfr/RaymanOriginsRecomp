// Driver de áudio do XAudio (#14, primeiro passo). O console chama o callback de
// cada cliente registrado a cada 5,333 ms (256 amostras a 48 kHz); o jogo responde
// com XAudioSubmitRenderDriverFrame. Sem esse ritmo o motor de áudio do jogo fica
// esperando. Semântica do Xenia (xboxkrnl_audio.cc, apu/audio_system.cc).
//
// Os frames recebidos ficam numa fila (6 canais x 256 amostras, float big-endian);
// a saída no host entra depois (SDL).
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <thread>
#include "apu/audio.h"
#include "cpu/guest_context.h"
#include "function.h"
#include "kernel/heap.h"
#include "kernel/memory_layout.h"
#include "kernel/thread.h"
#include "memory.h"

namespace
{
constexpr uint32_t DRIVER_TAG = 0x41550000;
constexpr size_t MAX_CLIENTS = 8;
constexpr auto PUMP_INTERVAL = std::chrono::microseconds(5333);
constexpr uint32_t XMA_MMIO_BASE = 0x7FEA0000;
constexpr size_t XMA_MMIO_SIZE = 0x10000;

struct Client
{
    bool inUse = false;
    uint32_t callback = 0;
    uint32_t wrappedArg = 0; // endereço do guest com o argumento (o callback recebe um ponteiro)
    std::chrono::steady_clock::time_point next{};
};

std::mutex g_mutex;
std::array<Client, MAX_CLIENTS> g_clients;
std::atomic<bool> g_workerStarted{ false };
AudioSink g_sink = nullptr;

void Worker()
{
    PPCContext ctx;
    if (InitMainThread(ctx) == nullptr)
    {
        fprintf(stderr, "[apu] não consegui inicializar o contexto da thread de áudio\n");
        return;
    }

    while (true)
    {
        uint32_t callback = 0, arg = 0;
        auto wake = std::chrono::steady_clock::now() + PUMP_INTERVAL;
        {
            std::lock_guard lock(g_mutex);
            Client* earliest = nullptr;
            for (auto& client : g_clients)
                if (client.inUse && (!earliest || client.next < earliest->next))
                    earliest = &client;
            if (earliest)
            {
                wake = earliest->next;
                callback = earliest->callback;
                arg = earliest->wrappedArg;
                auto now = std::chrono::steady_clock::now();
                earliest->next = std::max(earliest->next, now) + PUMP_INTERVAL;
            }
        }
        std::this_thread::sleep_until(wake);
        if (callback)
        {
            ctx.r3.u64 = arg;
            PPCFunc* function = g_memory.FindFunction(callback);
            if (function == nullptr)
            {
                fprintf(stderr, "[apu] callback 0x%08X sem função recompilada\n", callback);
                continue;
            }
            function(ctx, g_memory.base);
        }
    }
}

#if defined(__SWITCH__)
void* AudioWorkerEntry(void*)
{
    try
    {
        Worker();
    }
    catch (...)
    {
        fprintf(stderr, "[apu] worker terminou por exceção não tratada\n");
        ClearPPCContext();
    }
    return nullptr;
}

bool StartAudioWorker()
{
    pthread_t thread;
    const int createResult = pthread_create(&thread, nullptr, AudioWorkerEntry, nullptr);
    if (createResult != 0)
    {
        fprintf(stderr, "[apu] pthread_create(audio) falhou (%d)\n", createResult);
        return false;
    }

    const int detachResult = pthread_detach(thread);
    // Current devkitA64/libnx exposes pthread_create but has no Horizon
    // thread-detach syscall, so ENOSYS means the worker remains joinable for
    // the lifetime of the process. These workers are intentionally never
    // joined during normal title execution.
    if (detachResult != 0 && detachResult != ENOSYS)
    {
        fprintf(stderr, "[apu] pthread_detach(audio) falhou (%d)\n", detachResult);
        return false;
    }
    if (detachResult == ENOSYS)
        fprintf(stderr, "[apu] pthread_detach(audio) indisponível; worker mantido\n");
    return true;
}
#endif
} // namespace

bool InitAudioHardware()
{
    if (!g_memory.CommitRange(XMA_MMIO_BASE, XMA_MMIO_SIZE))
    {
        fprintf(stderr, "[apu] não consegui mapear a janela XMA MMIO 0x%08X..0x%08X\n",
                XMA_MMIO_BASE, XMA_MMIO_BASE + uint32_t(XMA_MMIO_SIZE));
        return false;
    }

    fprintf(stderr, "[apu] janela XMA MMIO mapeada em 0x%08X..0x%08X\n",
            XMA_MMIO_BASE, XMA_MMIO_BASE + uint32_t(XMA_MMIO_SIZE));
    return true;
}

void SetAudioSink(AudioSink sink)
{
    g_sink = sink;
}

static uint32_t XAudioRegisterRenderDriverClient(be<uint32_t>* callbackInfo, be<uint32_t>* driver)
{
    if (!callbackInfo || callbackInfo[0].get() == 0)
        return 0x80070057; // E_INVALIDARG

    std::lock_guard lock(g_mutex);
    for (size_t i = 0; i < MAX_CLIENTS; i++)
    {
        Client& client = g_clients[i];
        if (client.inUse)
            continue;
        auto* wrapped = static_cast<be<uint32_t>*>(g_runtimeHeap.AllocZeroed(4));
        *wrapped = callbackInfo[1].get();
        client = { true, callbackInfo[0].get(), g_memory.MapVirtual(wrapped), std::chrono::steady_clock::now() };
        driver->set(DRIVER_TAG | uint32_t(i));
        fprintf(stderr, "[apu] cliente %zu: callback 0x%08X arg 0x%08X\n", i, client.callback, callbackInfo[1].get());
        if (!g_workerStarted.exchange(true))
#if defined(__SWITCH__)
        {
            if (!StartAudioWorker())
                g_workerStarted = false;
        }
#else
            std::thread(Worker).detach();
#endif
        return 0;
    }
    return 0x8007000E; // E_OUTOFMEMORY
}

static uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver)
{
    std::lock_guard lock(g_mutex);
    size_t index = driver & 0xFFFF;
    if (index < MAX_CLIENTS && g_clients[index].inUse)
    {
        g_runtimeHeap.Free(g_memory.Translate(g_clients[index].wrappedArg));
        g_clients[index] = {};
    }
    return 0;
}

// 256 amostras x 6 canais (FL, FR, C, LFE, RL, RR), float big-endian, planar.
static uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, const be<float>* samples)
{
    // Diagnóstico: RAYMAN_AUDIO_DUMP=arquivo grava o estéreo (float32, 48 kHz) recebido.
    static FILE* dump = [] { const char* path = getenv("RAYMAN_AUDIO_DUMP"); return path ? fopen(path, "wb") : nullptr; }();
    if ((g_sink || dump) && samples)
    {
        float stereo[256 * 2];
        for (int i = 0; i < 256; i++)
        {
            float fl = samples[0 * 256 + i].get(), fr = samples[1 * 256 + i].get();
            float c = samples[2 * 256 + i].get();
            float rl = samples[4 * 256 + i].get(), rr = samples[5 * 256 + i].get();
            stereo[i * 2 + 0] = fl + 0.707f * c + 0.707f * rl;
            stereo[i * 2 + 1] = fr + 0.707f * c + 0.707f * rr;
        }
        if (g_sink)
            g_sink(stereo, 256);
        if (dump)
            fwrite(stereo, sizeof(stereo), 1, dump);
    }
    (void)driver;
    return 0;
}

static uint32_t XAudioGetSpeakerConfig(be<uint32_t>* config)
{
    config->set(0x00010001); // estéreo
    return 0;
}

static uint32_t XAudioGetVoiceCategoryVolume(uint32_t category, be<float>* volume)
{
    (void)category;
    volume->set(1.0f);
    return 0;
}


// ---- Contextos XMA (decodificador de hardware) ----
// 320 contextos de 64 bytes em memória física, como no Xenia. A decodificação em
// si (hardware XMA, disparado por MMIO) ainda não existe: os sons ficam mudos.
constexpr uint32_t XMA_CONTEXT_COUNT = 320;
constexpr uint32_t XMA_CONTEXT_SIZE = 64;
static std::mutex g_xmaMutex;
static uint32_t g_xmaBase = 0;
static std::array<bool, XMA_CONTEXT_COUNT> g_xmaUsed{};

static uint32_t XMACreateContext(be<uint32_t>* context)
{
    std::lock_guard lock(g_xmaMutex);
    if (g_xmaBase == 0)
        g_xmaBase = AllocatePhysicalMemory(XMA_CONTEXT_COUNT * XMA_CONTEXT_SIZE, 0x1000);
    for (uint32_t i = 0; i < XMA_CONTEXT_COUNT; i++)
    {
        if (g_xmaUsed[i])
            continue;
        g_xmaUsed[i] = true;
        uint32_t address = g_xmaBase + i * XMA_CONTEXT_SIZE;
        memset(g_memory.Translate(address), 0, XMA_CONTEXT_SIZE);
        context->set(address);
        return 0;
    }
    context->set(0);
    return 0xC0000017; // STATUS_NO_MEMORY
}

static uint32_t XMAReleaseContext(uint32_t context)
{
    std::lock_guard lock(g_xmaMutex);
    if (g_xmaBase && context >= g_xmaBase && context < g_xmaBase + XMA_CONTEXT_COUNT * XMA_CONTEXT_SIZE)
        g_xmaUsed[(context - g_xmaBase) / XMA_CONTEXT_SIZE] = false;
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__XMACreateContext, XMACreateContext);
GUEST_FUNCTION_HOOK(__imp__XMAReleaseContext, XMAReleaseContext);

GUEST_FUNCTION_HOOK(__imp__XAudioRegisterRenderDriverClient, XAudioRegisterRenderDriverClient);
GUEST_FUNCTION_HOOK(__imp__XAudioUnregisterRenderDriverClient, XAudioUnregisterRenderDriverClient);
GUEST_FUNCTION_HOOK(__imp__XAudioSubmitRenderDriverFrame, XAudioSubmitRenderDriverFrame);
GUEST_FUNCTION_HOOK(__imp__XAudioGetSpeakerConfig, XAudioGetSpeakerConfig);
GUEST_FUNCTION_HOOK(__imp__XAudioGetVoiceCategoryVolume, XAudioGetVoiceCategoryVolume);
