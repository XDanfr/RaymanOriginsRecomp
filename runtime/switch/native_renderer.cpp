#include "switch/native_renderer.h"

#if defined(RAYMAN_SWITCH_NATIVE_VULKAN)

#include "cpu/guest_context.h"
#include "memory.h"
#include "vk_renderer.h"

// libnx's pad helper collides with the runtime's platform-neutral PadState.
#define PadState LibnxPadState
#include <switch.h>
#undef PadState

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(
    VkInstance instance,
    const char* name);

namespace
{
constexpr uint32_t DISPLAY_WIDTH = 1280;
constexpr uint32_t DISPLAY_HEIGHT = 720;
constexpr uint32_t DEVICE_PIXEL_SHADER = 0x330C;
constexpr uint32_t DEVICE_VERTEX_SHADER = 0x3310;
constexpr uint32_t DEVICE_INDEX_BUFFER = 0x320C;
constexpr uint32_t TEXTURE_FETCH = 0x1C;
constexpr const char* SHADER_DIRECTORY =
    "sdmc:/switch/RaymanOriginsRecomp/shaders";

native::Renderer* g_renderer = nullptr;
std::unordered_map<uint32_t, uint64_t> g_shaders;
std::mutex g_rendererMutex;
std::atomic<bool> g_active{ false };
bool g_frameOpen = false;
uint64_t g_frames = 0;

uint32_t LoadBE32(uint32_t guest)
{
    uint32_t value = 0;
    std::memcpy(&value, g_memory.Translate(guest), sizeof(value));
    return __builtin_bswap32(value);
}

uint64_t ShaderHash(uint32_t object)
{
    const auto found = g_shaders.find(object);
    return found == g_shaders.end() ? 0 : found->second;
}

std::vector<uint32_t> LoadShader(uint64_t hash, bool vertex)
{
    char path[160];
    std::snprintf(
        path,
        sizeof(path),
        "%s/%016llX_%s.spv",
        SHADER_DIRECTORY,
        static_cast<unsigned long long>(hash),
        vertex ? "vs" : "ps");

    FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        fprintf(stderr, "[vulkan] shader ausente: %s\n", path);
        return {};
    }

    std::fseek(file, 0, SEEK_END);
    const long byteSize = std::ftell(file);
    std::rewind(file);
    if (byteSize <= 0 || (byteSize & 3) != 0)
    {
        std::fclose(file);
        fprintf(stderr, "[vulkan] SPIR-V inválido: %s\n", path);
        return {};
    }

    std::vector<uint32_t> words(static_cast<size_t>(byteSize) / sizeof(uint32_t));
    const size_t read = std::fread(words.data(), 1, static_cast<size_t>(byteSize), file);
    std::fclose(file);
    if (read != static_cast<size_t>(byteSize) || words[0] != 0x07230203u)
    {
        fprintf(stderr, "[vulkan] leitura SPIR-V falhou: %s\n", path);
        return {};
    }
    return words;
}

const uint8_t* ReadPhysicalMemory(uint32_t physical, uint32_t size)
{
    if (uint64_t(physical) + uint64_t(size) > 0x20000000ull)
        return nullptr;

    const uint32_t guest = 0xA0000000u + physical;
    if (!g_memory.IsRangeCommitted(guest, size))
        return nullptr;
    return static_cast<const uint8_t*>(g_memory.Translate(guest));
}

void OpenFrame()
{
    if (!g_frameOpen)
    {
        g_renderer->BeginFrame();
        g_frameOpen = true;
    }
}

bool RedirectStdout()
{
    fflush(stdout);
    if (dup2(fileno(stderr), fileno(stdout)) < 0)
        return false;
    clearerr(stdout);
    setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
    return true;
}
}

bool InitSwitchNativeRenderer()
{
    if (g_active.load(std::memory_order_acquire))
        return true;

    setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1);
    setenv("MESA_SHADER_CACHE_DISABLE", "1", 1);
    setenv("MESA_GLSL_CACHE_DISABLE", "1", 1);
    setenv("MESA_VK_ENABLE_SUBMIT_THREAD", "1", 1);

    consoleUpdate(nullptr);
    consoleExit(nullptr);

    NWindow* window = nwindowGetDefault();
    if (window == nullptr)
    {
        consoleInit(nullptr);
        fprintf(stderr, "[vulkan] nwindowGetDefault falhou\n");
        return false;
    }
    nwindowSetDimensions(window, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    nwindowSetCrop(window, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    nwindowSetSwapInterval(window, 1);

    auto* renderer = new native::Renderer();
    renderer->log = [](const char* stage) {
        fprintf(stderr, "[vulkan] %s\n", stage);
    };
    renderer->SetShaderSource(LoadShader);
    renderer->SetMemory(ReadPhysicalMemory);

    const auto getInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetInstanceProcAddr"));
    if (getInstanceProcAddr == nullptr)
    {
        fprintf(stderr, "[vulkan] NVK não forneceu vkGetInstanceProcAddr\n");
        delete renderer;
        consoleInit(nullptr);
        return false;
    }

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_NN_VI_SURFACE_EXTENSION_NAME,
    };
    native::Renderer::SurfaceFactory makeSurface = [window](VkInstance instance) {
        VkViSurfaceCreateInfoNN info{};
        info.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
        info.window = window;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (vkCreateViSurfaceNN(instance, &info, nullptr, &surface) != VK_SUCCESS)
            return VkSurfaceKHR(VK_NULL_HANDLE);
        return surface;
    };

    if (!renderer->Init(
            getInstanceProcAddr,
            extensions,
            std::move(makeSurface),
            DISPLAY_WIDTH,
            DISPLAY_HEIGHT))
    {
        fprintf(stderr, "[vulkan] inicialização falhou: %s\n", renderer->error().c_str());
        delete renderer;
        consoleInit(nullptr);
        return false;
    }

    if (!RedirectStdout())
    {
        fprintf(stderr, "[vulkan] não consegui redirecionar stdout\n");
        delete renderer;
        consoleInit(nullptr);
        return false;
    }

    g_renderer = renderer;
    g_active.store(true, std::memory_order_release);
    fprintf(stderr, "[vulkan] renderer NVK ativo em %ux%u\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    fprintf(stderr, "[vulkan] shaders: %s\n", SHADER_DIRECTORY);
    return true;
}

bool SwitchNativeRendererActive()
{
    return g_active.load(std::memory_order_acquire);
}

void RegisterSwitchNativeShader(uint32_t object, uint64_t hash)
{
    if (!SwitchNativeRendererActive() || object == 0 || hash == 0)
        return;
    std::lock_guard lock(g_rendererMutex);
    g_shaders[object] = hash;
}

void SubmitSwitchNativeDraw(uint32_t entry, const PPCContext& context)
{
    if (!SwitchNativeRendererActive())
        return;

    std::lock_guard lock(g_rendererMutex);
    const uint32_t device = context.r3.u32;
    const uint32_t vertexObject = LoadBE32(device + DEVICE_VERTEX_SHADER);
    const uint32_t pixelObject = LoadBE32(device + DEVICE_PIXEL_SHADER);
    const uint32_t indexBuffer = LoadBE32(device + DEVICE_INDEX_BUFFER);
    native::DrawCall call{
        static_cast<const uint8_t*>(g_memory.Translate(device + native::kStateBegin)),
        entry,
        context.r4.u32,
        context.r5.u32,
        context.r6.u32,
        context.r7.u32,
        ShaderHash(vertexObject),
        ShaderHash(pixelObject),
        indexBuffer ? LoadBE32(indexBuffer) : 0,
        indexBuffer ? LoadBE32(indexBuffer + 0x18) : 0,
    };
    if (entry == 2)
    {
        call.startIndex = 0;
        call.upData = static_cast<const uint8_t*>(g_memory.Translate(context.r6.u32));
    }
    OpenFrame();
    g_renderer->Draw(call);
}

void SubmitSwitchNativeClear(const PPCContext& context)
{
    if (!SwitchNativeRendererActive() || (context.r6.u32 & 1u) == 0)
        return;
    std::lock_guard lock(g_rendererMutex);
    OpenFrame();
    g_renderer->Clear(
        static_cast<const uint8_t*>(
            g_memory.Translate(context.r3.u32 + native::kStateBegin)),
        context.r7.u32);
}

void SubmitSwitchNativeResolve(const PPCContext& context)
{
    if (!SwitchNativeRendererActive() || context.r6.u32 == 0 || (context.r4.u32 & 4u))
        return;

    int32_t rectangle[4];
    const int32_t* rectanglePointer = nullptr;
    if (context.r5.u32 != 0)
    {
        for (uint32_t index = 0; index < 4; ++index)
            rectangle[index] = static_cast<int32_t>(LoadBE32(context.r5.u32 + index * 4));
        rectanglePointer = rectangle;
    }

    std::lock_guard lock(g_rendererMutex);
    OpenFrame();
    g_renderer->Resolve(
        static_cast<const uint8_t*>(
            g_memory.Translate(context.r3.u32 + native::kStateBegin)),
        rectanglePointer,
        static_cast<const uint8_t*>(
            g_memory.Translate(context.r6.u32 + TEXTURE_FETCH)));
}

void PresentSwitchNativeFrame()
{
    if (!SwitchNativeRendererActive())
        return;
    std::lock_guard lock(g_rendererMutex);
    if (!g_frameOpen)
        return;

    if (g_renderer->stale() && !g_renderer->Recreate())
        fprintf(stderr, "[vulkan] swapchain: %s\n", g_renderer->error().c_str());
    if (!g_renderer->EndFrame(nullptr))
        fprintf(stderr, "[vulkan] present: %s\n", g_renderer->error().c_str());
    g_frameOpen = false;

    if (++g_frames <= 3 || (g_frames % 300) == 0)
    {
        const auto& stats = g_renderer->stats();
        fprintf(stderr,
                "[vulkan] frame %llu: %u draws, %u ignorados, %u pipelines, %u texturas\n",
                static_cast<unsigned long long>(g_frames), stats.draws, stats.skipped,
                stats.pipelines, stats.textures);
        for (const auto& [reason, count] : g_renderer->TakeSkipReasons())
            fprintf(stderr, "[vulkan] ignorado x%u: %s\n", count, reason.c_str());
    }
}

#else

bool InitSwitchNativeRenderer() { return false; }
bool SwitchNativeRendererActive() { return false; }
void RegisterSwitchNativeShader(uint32_t, uint64_t) {}
void SubmitSwitchNativeDraw(uint32_t, const PPCContext&) {}
void SubmitSwitchNativeClear(const PPCContext&) {}
void SubmitSwitchNativeResolve(const PPCContext&) {}
void PresentSwitchNativeFrame() {}

#endif
