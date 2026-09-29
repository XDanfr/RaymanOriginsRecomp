#include "switch/presenter.h"

// libnx's pad helper collides with the runtime's platform-neutral PadState.
#define PadState LibnxPadState
#include <switch.h>
#undef PadState

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "memory.h"
#include "switch/native_renderer.h"

namespace
{
constexpr uint32_t DISPLAY_WIDTH = 1280;
constexpr uint32_t DISPLAY_HEIGHT = 720;
constexpr size_t FRONT_BUFFER_SAMPLE_SIZE = 64 * 1024;

constexpr uint32_t PackRgba8(uint32_t red, uint32_t green, uint32_t blue)
{
    return red | (green << 8) | (blue << 16) | (0xFFu << 24);
}

std::atomic<uint32_t> g_frontBuffer{ 0 };
std::atomic<uint32_t> g_width{ 0 };
std::atomic<uint32_t> g_height{ 0 };
std::atomic<uint64_t> g_submittedFrame{ 0 };

Framebuffer g_framebuffer{};
uint64_t g_presentedFrame = 0;
bool g_active = false;
bool g_failed = false;
bool g_consoleReleased = false;

bool RedirectStdoutAfterConsoleExit()
{
    // consoleExit destroys the software console framebuffer, but stdout keeps
    // its descriptor. Any later printf would otherwise re-enter libnx's
    // ConsoleSwRenderer_drawChar with a null framebuffer. stderr already owns
    // runtime.log, so make stdout share that open file description.
    fflush(stdout);
    if (dup2(fileno(stderr), fileno(stdout)) < 0)
    {
        fprintf(stderr, "[present] não consegui redirecionar stdout após consoleExit\n");
        return false;
    }

    clearerr(stdout);
    setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
    return true;
}

void RestoreConsoleAfterFailure()
{
    if (!g_consoleReleased)
        return;

    consoleInit(nullptr);
    g_consoleReleased = false;
}

bool StartPresenter()
{
    consoleUpdate(nullptr);
    consoleExit(nullptr);
    g_consoleReleased = true;

    NWindow* window = nwindowGetDefault();
    if (window == nullptr)
    {
        fprintf(stderr, "[present] nwindowGetDefault falhou\n");
        RestoreConsoleAfterFailure();
        return false;
    }

    Result result = nwindowSetDimensions(window, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (R_FAILED(result))
        fprintf(stderr, "[present] nwindowSetDimensions falhou: 0x%08X\n", result);
    result = nwindowSetCrop(window, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (R_FAILED(result))
        fprintf(stderr, "[present] nwindowSetCrop falhou: 0x%08X\n", result);
    result = nwindowSetSwapInterval(window, 1);
    if (R_FAILED(result))
        fprintf(stderr, "[present] nwindowSetSwapInterval falhou: 0x%08X\n", result);

    result = framebufferCreate(
        &g_framebuffer,
        window,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        PIXEL_FORMAT_RGBA_8888,
        2);
    if (R_FAILED(result))
    {
        fprintf(stderr, "[present] framebufferCreate falhou: 0x%08X\n", result);
        RestoreConsoleAfterFailure();
        return false;
    }

    result = framebufferMakeLinear(&g_framebuffer);
    if (R_FAILED(result))
    {
        fprintf(stderr, "[present] framebufferMakeLinear falhou: 0x%08X\n", result);
        framebufferClose(&g_framebuffer);
        std::memset(&g_framebuffer, 0, sizeof(g_framebuffer));
        RestoreConsoleAfterFailure();
        return false;
    }

    if (!RedirectStdoutAfterConsoleExit())
    {
        framebufferClose(&g_framebuffer);
        std::memset(&g_framebuffer, 0, sizeof(g_framebuffer));
        RestoreConsoleAfterFailure();
        return false;
    }

    g_active = true;
    fprintf(stderr, "[present] stdout redirecionado para runtime.log\n");
    fprintf(stderr, "[present] framebuffer libnx ativo em %ux%u RGBA8\n",
            DISPLAY_WIDTH, DISPLAY_HEIGHT);
    return true;
}

void AuditFrontBuffer(uint64_t frame, uint32_t physical, uint32_t width, uint32_t height)
{
    if (frame > 1 && (frame % 300) != 0)
        return;

    const uint32_t guest = 0xA0000000u + (physical & 0x1FFFFFFFu);
    const uint64_t requestedBytes = uint64_t(width) * uint64_t(height) * 4;
    const size_t sampleBytes = static_cast<size_t>(
        std::min<uint64_t>(requestedBytes, FRONT_BUFFER_SAMPLE_SIZE));
    if (sampleBytes == 0 || !g_memory.IsRangeCommitted(guest, sampleBytes))
    {
        fprintf(stderr,
                "[present] frame %llu: front buffer 0x%08X sem backing legível para auditoria\n",
                static_cast<unsigned long long>(frame), physical);
        return;
    }

    const auto* bytes = static_cast<const uint8_t*>(g_memory.Translate(guest));
    uint64_t hash = 0xCBF29CE484222325ull;
    size_t nonZero = 0;
    for (size_t index = 0; index < sampleBytes; ++index)
    {
        nonZero += bytes[index] != 0;
        hash = (hash ^ bytes[index]) * 0x100000001B3ull;
    }

    fprintf(stderr,
            "[present] frame %llu: front 0x%08X %ux%u, amostra %zu bytes, %zu não-zero, hash %016llX\n",
            static_cast<unsigned long long>(frame), physical, width, height,
            sampleBytes, nonZero, static_cast<unsigned long long>(hash));
}

void DrawDiagnosticFrame(uint64_t frame, uint32_t frontBuffer, uint32_t guestWidth, uint32_t guestHeight)
{
    uint32_t stride = 0;
    auto* pixels = static_cast<uint32_t*>(framebufferBegin(&g_framebuffer, &stride));
    if (pixels == nullptr || stride < DISPLAY_WIDTH * sizeof(uint32_t))
    {
        fprintf(stderr, "[present] framebufferBegin devolveu um buffer inválido\n");
        // libnx requires every dequeued frame to be paired, even if the
        // returned pointer or stride is unusable to this renderer.
        framebufferEnd(&g_framebuffer);
        return;
    }

    constexpr std::array<uint32_t, 8> bars = {
        PackRgba8(28, 32, 52),
        PackRgba8(35, 73, 132),
        PackRgba8(31, 144, 161),
        PackRgba8(64, 176, 111),
        PackRgba8(224, 188, 63),
        PackRgba8(225, 112, 54),
        PackRgba8(191, 63, 86),
        PackRgba8(111, 61, 145),
    };

    const size_t stridePixels = stride / sizeof(uint32_t);
    for (uint32_t y = 0; y < DISPLAY_HEIGHT; ++y)
    {
        uint32_t* row = pixels + size_t(y) * stridePixels;
        for (uint32_t x = 0; x < DISPLAY_WIDTH; ++x)
            row[x] = bars[(x * bars.size()) / DISPLAY_WIDTH];
    }

    // White framing proves the entire visible extent, while the cyan marker
    // moves from the guest frame number and proves that new XE_SWAP packets
    // continue to reach the libnx main thread.
    const uint32_t white = PackRgba8(245, 245, 245);
    const uint32_t cyan = PackRgba8(53, 226, 242);
    for (uint32_t y = 0; y < DISPLAY_HEIGHT; ++y)
    {
        uint32_t* row = pixels + size_t(y) * stridePixels;
        if (y < 8 || y >= DISPLAY_HEIGHT - 8)
            std::fill(row, row + DISPLAY_WIDTH, white);
        else
        {
            std::fill(row, row + 8, white);
            std::fill(row + DISPLAY_WIDTH - 8, row + DISPLAY_WIDTH, white);
        }
    }

    const uint32_t markerX = static_cast<uint32_t>((frame * 5) % (DISPLAY_WIDTH - 64));
    for (uint32_t y = DISPLAY_HEIGHT / 2 - 12; y < DISPLAY_HEIGHT / 2 + 12; ++y)
    {
        uint32_t* row = pixels + size_t(y) * stridePixels;
        std::fill(row + markerX, row + markerX + 64, cyan);
    }

    // Encode the guest front-buffer address and dimensions as 32 binary tiles.
    // This is diagnostic metadata, not an attempt to display the unrendered
    // Xenos surface.
    const uint32_t metadata = frontBuffer ^ (guestWidth << 16) ^ guestHeight;
    for (uint32_t bit = 0; bit < 32; ++bit)
    {
        const uint32_t color = (metadata & (1u << bit)) ? white : PackRgba8(8, 10, 16);
        const uint32_t x0 = 32 + bit * 24;
        for (uint32_t y = 48; y < 72; ++y)
        {
            uint32_t* row = pixels + size_t(y) * stridePixels;
            std::fill(row + x0, row + x0 + 16, color);
        }
    }

    framebufferEnd(&g_framebuffer);
}
}

void SubmitSwitchPresentationFrame(
    uint64_t frame,
    uint32_t frontBuffer,
    uint32_t width,
    uint32_t height)
{
    g_frontBuffer.store(frontBuffer, std::memory_order_relaxed);
    g_width.store(width, std::memory_order_relaxed);
    g_height.store(height, std::memory_order_relaxed);
    g_submittedFrame.store(frame, std::memory_order_release);
}

void PumpSwitchPresentation()
{
    if (SwitchNativeRendererActive())
        return;
    if (g_failed)
        return;

    const uint64_t frame = g_submittedFrame.load(std::memory_order_acquire);
    if (frame == 0 || frame == g_presentedFrame)
        return;

    if (!g_active && !StartPresenter())
    {
        g_failed = true;
        return;
    }

    const uint32_t frontBuffer = g_frontBuffer.load(std::memory_order_relaxed);
    const uint32_t width = g_width.load(std::memory_order_relaxed);
    const uint32_t height = g_height.load(std::memory_order_relaxed);
    AuditFrontBuffer(frame, frontBuffer, width, height);
    DrawDiagnosticFrame(frame, frontBuffer, width, height);
    g_presentedFrame = frame;
}

void ShutdownSwitchPresentation()
{
    if (g_active)
    {
        framebufferClose(&g_framebuffer);
        std::memset(&g_framebuffer, 0, sizeof(g_framebuffer));
        g_active = false;
    }
}

bool SwitchPresentationOwnsDisplay()
{
    return g_active || SwitchNativeRendererActive();
}
