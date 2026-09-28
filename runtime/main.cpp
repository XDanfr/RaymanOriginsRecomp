// RaymanPort — runtime do Rayman Origins recompilado.
// Carrega o XEX na memória do guest, cria a thread principal do guest e chama o
// entry point recompilado. Imports ainda não implementados só logam (stubs).
#include <cstdio>
#include <pthread.h>
#include <cstdlib>
#include <unistd.h>
#include "memory.h"
#include "loader.h"
#include "crash_handler.h"
#include "kernel/memory_layout.h"
#include "kernel/thread.h"
#include "host/platform.h"

// A thread principal do macOS tem só 8 MB de pilha; o código recompilado usa a
// pilha do host a cada chamada do guest, então rodamos o jogo numa thread maior.
constexpr size_t MAIN_THREAD_HOST_STACK = 64 * 1024 * 1024;

static uint32_t g_entryPoint = 0;

static void* GuestMain(void*)
{
    PPCContext ctx;
    InitMainThread(ctx);

    fprintf(stderr, "[main] chamando entry point 0x%08X\n", g_entryPoint);
    try
    {
        g_memory.FindFunction(g_entryPoint)(ctx, g_memory.base);
        fprintf(stderr, "[main] entry point retornou (r3=0x%08X)\n", ctx.r3.u32);
    }
    catch (...)
    {
        fprintf(stderr, "[main] thread principal encerrada pelo jogo\n");
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    setvbuf(stderr, nullptr, _IONBF, 0);
    const char* xexPath = argc > 1 ? argv[1] : "private/game/default.xex";

    if (!g_memory.Init())
        return 1;
    InstallCrashHandler();
    if (!InitGuestHeaps())
        return 1;

    LoadedImage image;
    if (!LoadXexImage(xexPath, g_memory.base, image))
        return 1;

    if (g_memory.FindFunction(image.entryPoint) == nullptr)
    {
        fprintf(stderr, "[main] entry point 0x%08X não tem função recompilada\n", image.entryPoint);
        return 1;
    }
    g_entryPoint = image.entryPoint;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, MAIN_THREAD_HOST_STACK);
    pthread_t thread;
    if (pthread_create(&thread, &attr, GuestMain, nullptr) != 0)
    {
        fprintf(stderr, "[main] não consegui criar a thread principal\n");
        return 1;
    }
    pthread_attr_destroy(&attr);

    // Janela e eventos na thread principal (exigência do macOS); o jogo roda na dele.
    if (getenv("RAYMAN_HEADLESS") == nullptr && InitPlatform())
    {
        RunPlatformLoop();
        ShutdownPlatform();
        _exit(0); // janela fechada: encerra sem esperar as threads do jogo
    }
    pthread_join(thread, nullptr);
    return 0;
}
