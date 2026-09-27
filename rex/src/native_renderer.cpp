// Native renderer, in-game. Draws every game frame with the native renderer
// (tools/native_renderer/vk_renderer.h: Vulkan + the game's SPIR-V shaders).
//   RAYMAN_NATIVE_RENDER=1     second window, next to the Xenos emulation (shadow mode)
//   RAYMAN_NATIVE_RENDER=main  the game's own window; run with --gpu_plugin=null so
//                              no GPU emulation runs and nothing else presents to it
//
// SPIR-V is read from RAYMAN_NATIVE_SPIRV (default: private/native/shaders_by_hash,
// relative to the repository root), as <HASH>_vs.spv / <HASH>_ps.spv.
#include <SDL3/SDL.h>
#if defined(__ANDROID__)
#include <android/native_window.h>
#include <dlfcn.h>
#endif
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_vulkan.h>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "vk_renderer.h"

#if defined(__ANDROID__)
#include <android/log.h>
#define NATIVE_LOG(...) __android_log_print(ANDROID_LOG_INFO, "RaymanNative", __VA_ARGS__)
#else
#define NATIVE_LOG(...) (std::fprintf(stderr, "[native] " __VA_ARGS__), std::fputc('\n', stderr))
#endif

extern uint8_t* g_rayman_physbase;  // native_capture.cpp
extern uint8_t* g_rayman_membase;   // hooks.cpp

namespace {

native::Renderer* g_renderer = nullptr;
SDL_Window* g_window = nullptr;
native::Renderer::SurfaceFactory g_makeSurface;
#if defined(__ANDROID__)
void* g_nativeWindow = nullptr;  // the ANativeWindow the surface was made from
#endif
bool g_frameOpen = false;
std::mutex g_mutex;
// The app is in the background (lock screen, notification shade, another app).
// Android keeps the window alive in some of these cases, so the game would go
// on rendering unseen and heat the phone up.
std::atomic<bool> g_background{false};

// Render scale (fraction of the screen size the game is drawn at, stretched to
// the screen): RAYMAN_RENDER_SCALE at start, then live from the app settings.
std::atomic<float> g_renderScale{[] {
  const char* v = std::getenv("RAYMAN_RENDER_SCALE");
  float s = v ? float(std::atof(v)) : 1.0f;
  return s > 0 ? s : 1.0f;
}()};

bool OnAppEvent(void*, SDL_Event* event) {
  if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) g_background = true;
  if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) g_background = false;
  return true;
}
double g_drawMs = 0;  // this frame: time in the draw hooks (vertex/constant copies, texture decode, pipelines)
std::chrono::steady_clock::time_point g_lastPresent = std::chrono::steady_clock::now();

struct ScopedTimer {
  double& total;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  ~ScopedTimer() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
};

bool Enabled() {
  static const bool enabled = std::getenv("RAYMAN_NATIVE_RENDER") != nullptr;
  return enabled;
}

std::string SpirvDir() {
  if (const char* dir = std::getenv("RAYMAN_NATIVE_SPIRV")) return dir;
  return "../private/native/spirv_ubo";
}

// Pipeline persistence, next to the SPIR-V folder: the driver's cache and the
// list of pipelines the game used, created up front on the next start.
std::string PipelineCachePath() { return SpirvDir() + "/../pipeline_cache.bin"; }
std::string PipelineListPath() { return SpirvDir() + "/../pipelines.txt"; }

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

std::vector<native::Renderer::PipelineDesc> ReadPipelineList() {
  std::vector<native::Renderer::PipelineDesc> out;
  std::ifstream f(PipelineListPath());
  unsigned long long vs, ps;
  uint32_t blend, stride;
  while (f >> std::hex >> vs >> ps >> blend >> std::dec >> stride) out.push_back({vs, ps, blend, stride});
  return out;
}

// Game thread: grabs the data, writes the files on a background thread.
void SavePipelines(native::Renderer* renderer) {
  auto data = renderer->PipelineCacheData();
  auto descs = renderer->PipelineDescs();
  std::thread([data = std::move(data), descs = std::move(descs)] {
    if (!data.empty()) {
      std::string tmp = PipelineCachePath() + ".tmp";
      if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
        std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
        std::rename(tmp.c_str(), PipelineCachePath().c_str());
      }
    }
    std::string tmp = PipelineListPath() + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "w")) {
      for (auto& p : descs)
        std::fprintf(f, "%016llx %016llx %x %u\n", (unsigned long long)p.vs, (unsigned long long)p.ps, p.blend, p.stride);
      std::fclose(f);
      std::rename(tmp.c_str(), PipelineListPath().c_str());
    }
  }).detach();
}

}  // namespace

#if defined(__ANDROID__)
#include <jni.h>
// Settings dialog (RaymanActivity): applies on the next frame.
extern "C" JNIEXPORT void JNICALL Java_io_github_belmantegu_raymanrecomp_RaymanActivity_nativeSetRenderScale(
    JNIEnv*, jclass, jfloat scale) {
  g_renderScale.store(scale, std::memory_order_relaxed);
}
#endif

// Main thread, after the runtime is set up: creates the window and the renderer.
void RaymanNativeRendererInit() {
  if (!Enabled() || g_renderer) {
    return;
  }
  NATIVE_LOG("native renderer: starting (%s)", std::getenv("RAYMAN_NATIVE_RENDER"));
  std::string mode = std::getenv("RAYMAN_NATIVE_RENDER");
  bool mainWindow = mode == "main";
  // "offscreen": no window at all (tests): draws into an image, captures read it back.
  bool offscreen = mode == "offscreen";
  std::vector<const char*> exts;
  PFN_vkGetInstanceProcAddr loader = nullptr;
  native::Renderer::SurfaceFactory makeSurface;
  if (mainWindow) {
    // The game's window was not created for Vulkan: make the surface from its
    // native handle, the way ReXGlue's presenter does.
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    g_window = count > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    if (!g_window) {
      NATIVE_LOG("no game window");
      return;
    }
    exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(__APPLE__)
    exts.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
    makeSurface = [](VkInstance instance) {
      SDL_MetalView view = SDL_Metal_CreateView(g_window);
      VkMetalSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
      info.pLayer = static_cast<const CAMetalLayer*>(SDL_Metal_GetLayer(view));
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      vkCreateMetalSurfaceEXT(instance, &info, nullptr, &surface);
      return surface;
    };
#elif defined(__ANDROID__)
    exts.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    makeSurface = [](VkInstance instance) {
      VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
      // Started behind the lock screen (or sent to the background right
      // away), the activity has no surface yet and the property is null. A
      // null window faults inside the driver, and the runtime's fault handler
      // retries the access forever: wait for the window instead.
      auto window = [] {
        return static_cast<ANativeWindow*>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
      };
      if (!window()) NATIVE_LOG("native renderer: no window yet (app in the background), waiting");
      while (!window()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      info.window = window();
      g_nativeWindow = info.window;
      // The game runs at 60 fps: ask for a 60 Hz display mode rather than 120.
      // ANativeWindow_setFrameRate is API 30+; look it up so API 29 still loads.
      using SetFrameRate = int32_t (*)(ANativeWindow*, float, int8_t);
      if (auto set = reinterpret_cast<SetFrameRate>(dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"))) {
        set(info.window, 60.0f, 1 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE */);
      }
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      vkCreateAndroidSurfaceKHR(instance, &info, nullptr, &surface);
      return surface;
    };
#endif
  } else if (!offscreen) {
    if (!SDL_Vulkan_GetVkGetInstanceProcAddr() && !SDL_Vulkan_LoadLibrary(nullptr)) {
      NATIVE_LOG("SDL could not load Vulkan: %s", SDL_GetError());
      return;
    }
    g_window = SDL_CreateWindow("Rayman Origins - native renderer", 960, 540,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
      NATIVE_LOG("window: %s", SDL_GetError());
      return;
    }
    Uint32 count = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&count);
    exts.assign(sdlExts, sdlExts + count);
    loader = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    makeSurface = [](VkInstance instance) {
      VkSurfaceKHR surface = VK_NULL_HANDLE;
      if (!SDL_Vulkan_CreateSurface(g_window, instance, nullptr, &surface)) return VkSurfaceKHR(VK_NULL_HANDLE);
      return surface;
    };
  }
  int w = 0, h = 0;
  if (offscreen) {
    // RAYMAN_OFFSCREEN_SIZE=WxH (default 1280x720).
    w = 1280, h = 720;
    if (const char* size = std::getenv("RAYMAN_OFFSCREEN_SIZE")) std::sscanf(size, "%dx%d", &w, &h);
  } else {
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
  }
  NATIVE_LOG("native renderer: window %p %dx%d", static_cast<void*>(g_window), w, h);
  auto* renderer = new native::Renderer();
  renderer->log = [](const char* stage) { NATIVE_LOG("native renderer: %s", stage); };
  g_makeSurface = makeSurface;
  renderer->SetPipelineCacheData(ReadFile(PipelineCachePath()));
  bool ok = renderer->Init(loader, exts, makeSurface, uint32_t(w), uint32_t(h));
  if (!ok) {
    NATIVE_LOG("renderer init failed: %s", renderer->error().c_str());
    delete renderer;
    return;
  }
  std::string dir = SpirvDir();
  renderer->SetShaderSource([dir](uint64_t hash, bool vertex) {
    char name[64];
    std::snprintf(name, sizeof(name), "/%016llX_%s.spv", (unsigned long long)hash, vertex ? "vs" : "ps");
    std::ifstream f(dir + name, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    if (words.empty()) NATIVE_LOG("missing SPIR-V %s", name);
    return words;
  });
  renderer->SetMemory([](uint32_t address, uint32_t size) -> const uint8_t* {
    if (!g_rayman_physbase || uint64_t(address) + size > 0x20000000u) return nullptr;
    return g_rayman_physbase + address;
  });
  if (const char* ws = std::getenv("RAYMAN_WIDESCREEN")) {
    float aspect = std::string(ws) == "auto" ? float(w) / float(h) : float(std::atof(ws));
    if (aspect > 16.0f / 9.0f + 0.01f && g_rayman_membase) {
      // UbiArt fits its camera and screen rects to 16:9 with these two
      // constants (sub_824C8798): 16/9 at 0x8201EF58 and 9/16 at 0x8201EF5C.
      // They live in the image's read-only data, so the page is made writable
      // for the store and read-only again afterwards.
      auto store = [](uint32_t address, float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        bits = __builtin_bswap32(bits);
        uint8_t* host = g_rayman_membase + address;
        uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
        void* start = reinterpret_cast<void*>(uintptr_t(host) & ~(page - 1));
        mprotect(start, page, PROT_READ | PROT_WRITE);
        std::memcpy(host, &bits, 4);
        mprotect(start, page, PROT_READ);
      };
      store(0x8201EF58, aspect);
      store(0x8201EF5C, 1.0f / aspect);
      renderer->SetAspect(aspect);
      NATIVE_LOG("widescreen: aspect %.4f", aspect);
    }
  }
  {
    auto start = std::chrono::steady_clock::now();
    auto known = ReadPipelineList();
    uint32_t made = renderer->Prewarm(known);
    NATIVE_LOG("pipelines: %u of %zu known created up front in %.0f ms", made, known.size(),
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  }
  g_renderer = renderer;
  SDL_AddEventWatch(OnAppEvent, nullptr);
  NATIVE_LOG("native renderer ready (%dx%d), SPIR-V from %s", w, h, dir.c_str());
}

namespace {
void OpenFrame() {
  if (!g_frameOpen) {
    g_renderer->BeginFrame();
    g_frameOpen = true;
  }
}
}  // namespace

// Render thread, from the draw hooks (native_capture.cpp).
void RaymanNativeRendererDraw(const native::DrawCall& call) {
  if (!g_renderer) {
    return;
  }
  std::lock_guard lock(g_mutex);
  ScopedTimer timer{g_drawMs};
  OpenFrame();
  g_renderer->Draw(call);
}

// Render thread, from the Clear / Resolve hooks. state = device + native::kStateBegin.
void RaymanNativeRendererClear(const uint8_t* state, uint32_t argb) {
  if (!g_renderer) {
    return;
  }
  std::lock_guard lock(g_mutex);
  OpenFrame();
  g_renderer->Clear(state, argb);
}

void RaymanNativeRendererResolve(const uint8_t* state, const int32_t* rect, const uint8_t* destFetch) {
  if (!g_renderer) {
    return;
  }
  std::lock_guard lock(g_mutex);
  OpenFrame();
  g_renderer->Resolve(state, rect, destFetch);
}

// Render thread, from the Present hook.
void RaymanNativeRendererPresent() {
  if (!g_renderer) {
    return;
  }
  std::lock_guard lock(g_mutex);
  if (!g_frameOpen) {
    return;
  }
  // With RAYMAN_CAPTURE=1, save the native frame every 10 s next to the
  // emulated captures (captures/native_NNN.ppm).
  static auto start = std::chrono::steady_clock::now();
  static const int interval = std::getenv("RAYMAN_CAPTURE_INTERVAL") ? std::atoi(std::getenv("RAYMAN_CAPTURE_INTERVAL")) : 10;
  static int nextShot = interval;
  bool shot = std::getenv("RAYMAN_CAPTURE") &&
              std::chrono::steady_clock::now() - start >= std::chrono::seconds(nextShot);
  std::vector<uint8_t> pixels;
#if defined(__ANDROID__)
  // In the background SDL drops the window (the property goes null) and hands
  // out a new one when the app comes back: skip frames meanwhile, then build a
  // surface and swapchain for the new window. Behind the lock screen or the
  // notification shade the window can stay: hold the game there too.
  auto currentWindow = [] {
    return SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
  };
  void* window = currentWindow();
  if (!window || g_background) {
    g_renderer->DiscardFrame();
    g_frameOpen = false;
    // Hold the game until it is visible again: its logic advances once per
    // presented frame, so it pauses instead of running unseen.
    NATIVE_LOG("native renderer: app in the background, game held");
    while (g_background || !currentWindow()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    NATIVE_LOG("native renderer: back in the foreground");
    g_lastPresent = std::chrono::steady_clock::now();
    g_drawMs = 0;
    return;
  }
  if (window != g_nativeWindow) {
    NATIVE_LOG("native renderer: new window %p, recreating the surface", window);
    if (!g_renderer->Recreate(g_makeSurface)) NATIVE_LOG("recreate failed: %s", g_renderer->error().c_str());
  }
#endif
  if (g_renderer->stale()) {
    NATIVE_LOG("native renderer: swapchain out of date, re-creating");
    if (!g_renderer->Recreate()) NATIVE_LOG("swapchain: %s", g_renderer->error().c_str());
  }
  float scale = g_renderScale.load(std::memory_order_relaxed);
  if (scale != g_renderer->renderScale()) {
    g_renderer->SetRenderScale(scale);
    NATIVE_LOG("render scale %.0f%% (%.0fx%.0f)", g_renderer->renderScale() * 100,
               g_renderer->width() * g_renderer->renderScale(), g_renderer->height() * g_renderer->renderScale());
  }
  g_renderer->EndFrame(shot ? &pixels : nullptr);
  g_frameOpen = false;
  if (shot && !pixels.empty()) {
    char name[64];
    std::snprintf(name, sizeof(name), "captures/native_%03d.ppm", nextShot);
    if (FILE* f = std::fopen(name, "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", g_renderer->width(), g_renderer->height());
      for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::fwrite(&pixels[i], 1, 3, f);
      std::fclose(f);
      NATIVE_LOG("saved %s", name);
    }
    nextShot += interval;
  }
  // Frame budget breakdown. Slow frames are logged with what they created, so
  // hitches can be told apart: CPU in the draw hooks (decode, new pipelines),
  // the end of the frame (recording, acquire), or waiting on the GPU.
  auto now = std::chrono::steady_clock::now();
  double frameMs = std::chrono::duration<double, std::milli>(now - g_lastPresent).count();
  g_lastPresent = now;
  auto& st = g_renderer->stats();
  static double sumFrame = 0, sumDraw = 0, sumRecord = 0, sumWait = 0, worst = 0;
  static uint32_t sumDraws = 0;
  sumFrame += frameMs, sumDraw += g_drawMs, sumRecord += st.recordMs, sumWait += st.waitMs, sumDraws += st.draws;
  worst = std::max(worst, frameMs);
  if (frameMs > 20.0)
    NATIVE_LOG("slow frame %.1f ms: draw hooks %.1f, end %.1f, gpu wait %.1f | %u draws, +%u pipelines, +%u textures, "
               "%u re-uploads, %zu KB uploaded",
               frameMs, g_drawMs, st.recordMs, st.waitMs, st.draws, st.newPipelines, st.newTextures, st.reuploads,
               st.uploadBytes >> 10);
  g_drawMs = 0;
  // New pipelines since the last save: persist them (at most every 5 s).
  static uint32_t savedPipelines = st.pipelines;
  static auto lastSave = now;
  if (st.pipelines != savedPipelines && now - lastSave > std::chrono::seconds(5)) {
    SavePipelines(g_renderer);
    savedPipelines = st.pipelines;
    lastSave = now;
  }
  static int frames = 0;
  if (++frames % 120 == 0) {
    NATIVE_LOG("perf over 120 frames: %.1f fps, avg frame %.1f ms (draw hooks %.1f, end %.1f, gpu wait %.1f), "
               "worst %.1f ms, %u draws/frame",
               120000.0 / sumFrame, sumFrame / 120, sumDraw / 120, sumRecord / 120, sumWait / 120, worst, sumDraws / 120);
    sumFrame = sumDraw = sumRecord = sumWait = worst = 0;
    sumDraws = 0;
  }
  if (frames % 300 == 0) {
    auto& s = g_renderer->stats();
    NATIVE_LOG("native frame %d: %u draws, %u skipped, %u pipelines, %u textures", frames, s.draws, s.skipped,
               s.pipelines, s.textures);
    for (auto& [reason, n] : g_renderer->TakeSkipReasons()) NATIVE_LOG("  skipped x%u: %s", n, reason.c_str());
    for (auto& [note, n] : g_renderer->TakeNotes()) NATIVE_LOG("  note x%u: %s", n, note.c_str());
  }
}
