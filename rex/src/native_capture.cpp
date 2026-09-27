// Native renderer, step 1: observe what the game draws through its D3D layer.
//
// With RAYMAN_NATIVE_CAPTURE=1 the hooks below log:
//  - every shader the game creates, with the XXH3 hash XenosRecomp uses to key
//    its precompiled SPIR-V cache (hash of the container: function[1] + function[2] bytes);
//  - a per-frame summary of the draw calls: which draw entry point, primitive
//    type, and the vertex/pixel shader bound in the device at that moment.
// Everything is passed through to the original functions, so the Xenos
// emulation keeps rendering. Addresses and device offsets: docs/D3D_MAP.md.
#include <rex/hook.h>

#include "vk_renderer.h"  // native::DrawCall

void RaymanNativeRendererDraw(const native::DrawCall& call);  // native_renderer.cpp
void RaymanNativeRendererClear(const uint8_t* state, uint32_t argb);
void RaymanNativeRendererResolve(const uint8_t* state, const int32_t* rect, const uint8_t* destFetch);

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#define CAPTURE_LOG(...) __android_log_print(ANDROID_LOG_INFO, "RaymanNative", __VA_ARGS__)
#else
#define CAPTURE_LOG(...) (std::fprintf(stderr, "[native] " __VA_ARGS__), std::fputc('\n', stderr))
#endif

extern uint8_t* g_rayman_membase;  // hooks.cpp
uint8_t* g_rayman_physbase = nullptr;  // set in RaymanApp::OnPostSetup

namespace {

constexpr uint32_t kDeviceVertexShader = 0x3310;
constexpr uint32_t kDevicePixelShader = 0x330C;

bool Enabled() {
  static const bool enabled = std::getenv("RAYMAN_NATIVE_CAPTURE") != nullptr ||
                              std::getenv("RAYMAN_NATIVE_DUMP") != nullptr ||
                              std::getenv("RAYMAN_NATIVE_RENDER") != nullptr;
  return enabled;
}

uint32_t LoadBE32(uint32_t guest_address) {
  uint32_t value;
  std::memcpy(&value, g_rayman_membase + guest_address, 4);
  return __builtin_bswap32(value);
}

struct ShaderInfo {
  uint64_t hash;
  bool vertex;
  std::vector<uint8_t> container;  // copied at creation: the game frees its buffer
};

struct DrawKey {
  int entry;          // 0 = DrawIndexed (826D7588), 1 = Draw (826D7170), 2 = DrawVerticesUP (826D7128)
  uint32_t primitive;
  uint64_t vs, ps;
  bool operator<(const DrawKey& o) const {
    return std::tie(entry, primitive, vs, ps) < std::tie(o.entry, o.primitive, o.vs, o.ps);
  }
};

std::mutex g_mutex;
FILE* g_dump = nullptr;  // open while a frame is being dumped
std::unordered_map<uint32_t, ShaderInfo> g_shaders;  // guest shader object -> info
std::map<DrawKey, int> g_frameDraws;
int g_frameDrawCount = 0;
int g_unknownShaderDraws = 0;
uint64_t g_frame = 0;

uint64_t HashShader(uint32_t function) {
  // Container header (big-endian words): [1] + [2] = bytes XenosRecomp hashes.
  uint32_t size = LoadBE32(function + 4) + LoadBE32(function + 8);
  return XXH3_64bits(g_rayman_membase + function, size);
}

void RecordShader(uint32_t object, uint32_t function, bool vertex) {
  if (!object || !function) {
    return;
  }
  uint32_t size = LoadBE32(function + 4) + LoadBE32(function + 8);
  if (size > (1u << 20)) {
    return;  // not a shader container
  }
  uint64_t hash = XXH3_64bits(g_rayman_membase + function, size);
  std::lock_guard lock(g_mutex);
  g_shaders[object] = {hash, vertex,
                       std::vector<uint8_t>(g_rayman_membase + function,
                                            g_rayman_membase + function + size)};
  CAPTURE_LOG("%s shader 0x%08X hash %016llX", vertex ? "vertex" : "pixel", object,
              (unsigned long long)hash);
}

uint64_t ShaderHash(uint32_t object) {
  auto it = g_shaders.find(object);
  return it == g_shaders.end() ? 0 : it->second.hash;
}

void DumpDraw(int entry, const PPCContext& ctx);

// Diagnostic (RAYMAN_NATIVE_FETCH_LOG): the vertex layout D3D really uses.
// XenosRecomp shaders come from the container, whose vertex fetches have
// offset, format and stride zeroed: D3D fills them in per vertex declaration
// ("bindings": vs + [vs + 896 + 8i], microcode at [binding + 872] + [vs + 32],
// sub_826E9B28). Logs each binding's fetches once per vertex shader and stride.
void LogVertexFetches(uint32_t device) {
  static const bool on = std::getenv("RAYMAN_NATIVE_FETCH_LOG") != nullptr;
  static std::set<uint64_t> logged;
  if (!on || !g_rayman_physbase) return;
  uint32_t vs = LoadBE32(device + kDeviceVertexShader);
  uint32_t stride = uint32_t(g_rayman_membase[device + 0x3268]) * 4;
  if (!vs) return;
  {
    // Log again whenever the filled-in microcode changes.
    uint32_t off = LoadBE32(vs + 896);
    if (!off || off > 0x100000) return;
    uint32_t code = LoadBE32(vs + off + 872) + LoadBE32(vs + 32), size = std::min(LoadBE32(vs + off + 876), 4096u);
    uint32_t phys = (code & 0x1FFFFFFF) + (code >= 0xE0000000u ? 0x1000 : 0);
    uint64_t sig = XXH3_64bits(g_rayman_physbase + phys, size) ^ (uint64_t(vs) << 32) ^ stride;
    if (!logged.insert(sig).second) return;
  }
  uint64_t hash;
  {
    std::lock_guard lock(g_mutex);
    hash = ShaderHash(vs);
  }
  uint32_t codeBase = LoadBE32(vs + 32);
  CAPTURE_LOG("fetch: vs %016llX object %08X stride %u code base %08X", (unsigned long long)hash, vs, stride,
              codeBase);
  for (uint32_t i = 0; i < 8; ++i) {
    uint32_t off = LoadBE32(vs + 896 + 8 * i), extra = LoadBE32(vs + 900 + 8 * i);
    if (!off || off > 0x100000) break;
    uint32_t binding = vs + off;
    uint32_t code = LoadBE32(binding + 872) + codeBase, size = LoadBE32(binding + 876);
    CAPTURE_LOG("  binding %u: +%X (%08X) code %08X size %u", i, off, extra, code, size);
    uint32_t phys = (code & 0x1FFFFFFF) + (code >= 0xE0000000u ? 0x1000 : 0);
    uint32_t bytes = std::min(size, 4096u) & ~3u;
    for (uint32_t o = 0; o + 12 <= bytes; o += 4) {
      uint32_t w[3];
      for (int k = 0; k < 3; ++k) {
        std::memcpy(&w[k], g_rayman_physbase + phys + o + k * 4, 4);
        w[k] = __builtin_bswap32(w[k]);
      }
      // vfetch: opcode 0, must_be_one, fetch constant 95 - stream (stream 0: 31*3+2).
      if ((w[0] & 0x1F) || !((w[0] >> 19) & 1) || ((w[0] >> 20) & 0x7F) != (31 | 2 << 5)) continue;
      CAPTURE_LOG("    @%03X dst r%u swiz %03X format %u offset %d stride %u mini %u", o, (w[0] >> 12) & 0x3F,
                  w[1] & 0xFFF, (w[1] >> 16) & 0x3F, int32_t(w[2] << 1) >> 9, w[2] & 0xFF, (w[1] >> 30) & 1);
    }
  }
}

void RecordDraw(int entry, const PPCContext& ctx) {
  uint32_t device = ctx.r3.u32;
  uint32_t primitive = ctx.r4.u32;
  {
    uint32_t ib = LoadBE32(device + 0x320C);
    uint64_t vsHash, psHash;
    {
      std::lock_guard lock(g_mutex);
      vsHash = ShaderHash(LoadBE32(device + kDeviceVertexShader));
      psHash = ShaderHash(LoadBE32(device + kDevicePixelShader));
    }
    native::DrawCall call{g_rayman_membase + device + native::kStateBegin, uint32_t(entry), primitive,
                          ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, vsHash, psHash,
                          ib ? LoadBE32(ib) : 0, ib ? LoadBE32(ib + 0x18) : 0};
    if (entry == 2) {
      // DrawVerticesUP(device, prim, vertexCount, data, stride): count and
      // stride go in baseVertex / indexCount, the vertices by pointer.
      call.startIndex = 0;
      call.upData = g_rayman_membase + ctx.r6.u32;
    }
    RaymanNativeRendererDraw(call);
  }
  if (g_dump) {
    std::lock_guard lock(g_mutex);
    DumpDraw(entry, ctx);
  }
  uint32_t vs = LoadBE32(device + kDeviceVertexShader);
  uint32_t ps = LoadBE32(device + kDevicePixelShader);
  std::lock_guard lock(g_mutex);
  DrawKey key{entry, primitive, ShaderHash(vs), ShaderHash(ps)};
  if ((vs && !key.vs) || (ps && !key.ps)) {
    ++g_unknownShaderDraws;
  }
  ++g_frameDraws[key];
  ++g_frameDrawCount;
}


// ---- Frame dump (RAYMAN_NATIVE_DUMP=<frame>) ----
// Writes captures/native_frame.bin: every draw of that frame with its device
// state, then the guest memory the draws read (indices, vertex buffers,
// textures) and the shader containers. Offline input for the native renderer
// (tools/native_renderer). The file holds game data: keep it out of git.
constexpr uint32_t kDumpBegin = 0x480, kDumpEnd = 0x3700;
constexpr uint32_t kDeviceIndexBuffer = 0x320C;

uint32_t g_dumpDraws = 0;
std::map<uint32_t, uint32_t> g_dumpRanges;  // physical address -> size

void Put32(uint32_t v) { std::fwrite(&v, 4, 1, g_dump); }
void Put64(uint64_t v) { std::fwrite(&v, 8, 1, g_dump); }

uint32_t ToPhysical(uint32_t guest) {
  return (guest & 0x1FFFFFFF) + (guest >= 0xE0000000u ? 0x1000u : 0u);
}

void AddRange(uint32_t physical, uint32_t size) {
  if (!size || size > 16u << 20 || physical >= 0x20000000u) {
    return;
  }
  auto& current = g_dumpRanges[physical];
  current = std::max(current, size);
}

void DumpDraw(int entry, const PPCContext& ctx) {
  uint32_t device = ctx.r3.u32;
  uint32_t vs = LoadBE32(device + kDeviceVertexShader);
  uint32_t ps = LoadBE32(device + kDevicePixelShader);
  uint32_t ib = LoadBE32(device + kDeviceIndexBuffer);
  uint32_t ibWord0 = ib ? LoadBE32(ib) : 0;
  uint32_t ibAddress = ib ? LoadBE32(ib + 0x18) : 0;
  Put32(0x57415244);  // 'DRAW'
  Put32(entry);
  Put32(ctx.r4.u32); Put32(ctx.r5.u32); Put32(ctx.r6.u32); Put32(ctx.r7.u32); Put32(ctx.r8.u32);
  Put64(ShaderHash(vs)); Put64(ShaderHash(ps));
  Put32(ib); Put32(ibWord0); Put32(ibAddress);
  std::fwrite(g_rayman_membase + device + kDumpBegin, 1, kDumpEnd - kDumpBegin, g_dump);
  ++g_dumpDraws;

  if (entry == 0 && ibAddress) {
    uint32_t indexSize = (ibWord0 & 0x80000000u) ? 4 : 2;
    AddRange(ToPhysical(ibAddress) + ctx.r6.u32 * indexSize, ctx.r7.u32 * indexSize);
  }
  // Fetch constants: vertex fetch (type 3, 2 dwords) and texture fetch (type 2, 6 dwords).
  // D3D binds stream s to vertex fetch 95 - s and sampler t to texture fetch t.
  for (uint32_t v = 80; v < 96; ++v) {
    uint32_t d0 = LoadBE32(device + kDumpBegin + v * 8);
    uint32_t d1 = LoadBE32(device + kDumpBegin + v * 8 + 4);
    if ((d0 & 3) == 3) {
      AddRange(d0 & 0x1FFFFFFC, ((d1 >> 2) & 0xFFFFFF) * 4);
    }
  }
  for (uint32_t t = 0; t < 16; ++t) {
    uint32_t base = device + kDumpBegin + t * 24;
    uint32_t d0 = LoadBE32(base), d1 = LoadBE32(base + 4), d2 = LoadBE32(base + 8);
    uint32_t d5 = LoadBE32(base + 20);
    if ((d0 & 3) != 2) {
      continue;
    }
    uint32_t width = (d2 & 0x1FFF) + 1, height = ((d2 >> 13) & 0x1FFF) + 1;
    // Bytes per 4x4 block by format: DXT1 8, DXT2-5 16, 8-bit 16, 16-bit 32, else 64.
    uint32_t format = d1 & 0x3F;
    uint32_t block = format == 0x12 ? 8 : (format == 0x13 || format == 0x14) ? 16 :
                     format == 0x02 ? 16 : (format == 0x04 || format == 0x05) ? 32 : 64;
    // Tiled textures take whole 32x32-block tiles: for block formats that is
    // 128x128 texels (a 512x64 DXT texture spans 128 texel rows, not 64).
    uint32_t bytes = ((width + 127) & ~127u) / 4 * (((height + 127) & ~127u) / 4) * block;
    AddRange(d1 & 0xFFFFF000, bytes);
    if (d5 & 0xFFFFF000) {
      AddRange(d5 & 0xFFFFF000, bytes);
    }
  }
}

void FinishDump() {
  uint64_t total = 0;
  for (auto& [physical, size] : g_dumpRanges) {
    if (total + size > (1ull << 30)) {
      CAPTURE_LOG("frame dump: 1 GB cap reached, skipping 0x%08X (%u bytes)", physical, size);
      continue;
    }
    total += size;
    Put32(0x204D454D);  // 'MEM '
    Put32(physical); Put32(size);
    std::fwrite(g_rayman_physbase + physical, 1, size, g_dump);
  }
  for (auto& [object, info] : g_shaders) {
    Put32(0x52444853);  // 'SHDR'
    Put64(info.hash); Put32(info.vertex ? 1 : 0); Put32(uint32_t(info.container.size()));
    std::fwrite(info.container.data(), 1, info.container.size(), g_dump);
  }
  Put32(0x20444E45);  // 'END '
  std::fclose(g_dump);
  g_dump = nullptr;
  CAPTURE_LOG("frame dump written: %u draws, %zu memory ranges (%.1f MB), %zu shaders", g_dumpDraws,
              g_dumpRanges.size(), total / 1048576.0, g_shaders.size());
}

}  // namespace

// Called from the Present hook (hooks.cpp) once per game frame.
void RaymanNativeCaptureFrame() {
  if (!Enabled()) {
    return;
  }
  std::lock_guard lock(g_mutex);
  ++g_frame;
  if (g_dump) {
    FinishDump();
  }
  // Trigger: RAYMAN_NATIVE_DUMP=<frame>, or creating captures/dump_now while
  // the game runs (checked every 30 frames; the file is removed when used).
  bool trigger = false;
  if (const char* target = std::getenv("RAYMAN_NATIVE_DUMP")) {
    trigger = g_frame + 1 == std::strtoull(target, nullptr, 10);
  }
  // RAYMAN_CAPTURE_DIR moves captures/ out of the working directory (Android
  // sets it to the app's external files folder, reachable with adb).
  const char* dirEnv = std::getenv("RAYMAN_CAPTURE_DIR");
  std::filesystem::path dir = dirEnv ? std::filesystem::path(dirEnv) / "captures" : "captures";
  if (dirEnv && g_frame == 1) {
    // Created by the game, so it owns it: on Android it cannot look inside a
    // folder that adb created, only drop files into one it owns.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
  }
  if (!trigger && g_frame % 30 == 0) {
    // Fires once per new dump_now: on Android the app may not be allowed to
    // delete a file adb created in its folder, so a changed time counts too.
    static std::filesystem::file_time_type lastTrigger;
    std::error_code ec;
    auto when = std::filesystem::last_write_time(dir / "dump_now", ec);
    if (!ec && when != lastTrigger) {
      lastTrigger = when;
      trigger = true;
      std::filesystem::remove(dir / "dump_now", ec);
    }
  }
  if (trigger && g_rayman_physbase) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    g_dump = std::fopen((dir / "native_frame.bin").string().c_str(), "wb");
    if (g_dump) {
      std::fwrite("RNF1", 1, 4, g_dump);
      g_dumpDraws = 0;
      g_dumpRanges.clear();
    }
  }
  if (g_frame % 120 == 0) {
    CAPTURE_LOG("frame %llu: %d draws, %zu distinct (entry/prim/shaders), %d with an unknown shader, %zu shaders known",
                (unsigned long long)g_frame, g_frameDrawCount, g_frameDraws.size(),
                g_unknownShaderDraws, g_shaders.size());
    for (auto& [key, count] : g_frameDraws) {
      CAPTURE_LOG("  entry %d prim %u vs %016llX ps %016llX x%d", key.entry, key.primitive,
                  (unsigned long long)key.vs, (unsigned long long)key.ps, count);
    }
  }
  g_frameDraws.clear();
  g_frameDrawCount = 0;
  g_unknownShaderDraws = 0;
}

REX_EXTERN(__imp__sub_826CD850);  // CreateVertexShader(function) -> shader
REX_EXTERN(__imp__sub_826CD668);  // CreatePixelShader(function) -> shader
REX_EXTERN(__imp__sub_826D7588);  // DrawIndexed
REX_EXTERN(__imp__sub_826D7170);  // Draw
REX_EXTERN(__imp__sub_826D7128);  // DrawVerticesUP(prim, vertexCount, data, stride)
REX_EXTERN(__imp__sub_826D9588);  // Resolve
REX_EXTERN(__imp__sub_826D6B98);  // Clear

REX_HOOK_RAW(sub_826CD850) {
  uint32_t function = ctx.r3.u32;
  __imp__sub_826CD850(ctx, base);
  if (Enabled()) {
    RecordShader(ctx.r3.u32, function, true);
  }
}

REX_HOOK_RAW(sub_826CD668) {
  uint32_t function = ctx.r3.u32;
  __imp__sub_826CD668(ctx, base);
  if (Enabled()) {
    RecordShader(ctx.r3.u32, function, false);
  }
}

REX_HOOK_RAW(sub_826D7588) {
  if (Enabled()) {
    RecordDraw(0, ctx);
  }
  uint32_t device = ctx.r3.u32;
  __imp__sub_826D7588(ctx, base);
  if (Enabled()) LogVertexFetches(device);
}

REX_HOOK_RAW(sub_826D7170) {
  if (Enabled()) {
    RecordDraw(1, ctx);
  }
  __imp__sub_826D7170(ctx, base);
}

// Not BeginVertices (826D6C68): it only reserves command-buffer space, and the
// caller copies the vertices after it returns. The public wrapper has them.
REX_HOOK_RAW(sub_826D7128) {
  if (Enabled()) {
    RecordDraw(2, ctx);
  }
  __imp__sub_826D7128(ctx, base);
}

// Render-target traffic, logged with RAYMAN_NATIVE_CAPTURE (frames multiple of 120).
// Resolve(device, flags, srcRect, destTexture, destPoint, level, slice, clearColor, ...):
// the destination is a D3DBaseTexture: the D3DResource header (Common,
// ReferenceCount, Fence, ReadFence, Identifier, BaseFlush: 0x18 bytes), then
// MipFlush, then the fetch constant at texture + 0x1C. (Reading it at + 0x18
// shifted every field by a dword: resolved textures landed at a wrong address
// and size, and whatever sampled them, like the water's refraction, read an
// empty texture.) The source is the current render target: RB_SURFACE_INFO /
// RB_COLOR_INFO at device + 0x2880 / 0x2884.
constexpr uint32_t kTextureFetch = 0x1C;

REX_HOOK_RAW(sub_826D9588) {
  if (Enabled()) {
    uint32_t device = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, tex = ctx.r6.u32;
    // Color resolves only (bit 2: depth/stencil), into a texture.
    if (tex && !(flags & 4)) {
      int32_t r[4];
      if (rect) for (int i = 0; i < 4; ++i) r[i] = int32_t(LoadBE32(rect + 4 * i));
      RaymanNativeRendererResolve(g_rayman_membase + device + native::kStateBegin, rect ? r : nullptr,
                                  g_rayman_membase + tex + kTextureFetch);
    }
  }
  if (std::getenv("RAYMAN_NATIVE_CAPTURE") && g_frame % 120 == 0) {
    uint32_t device = ctx.r3.u32, tex = ctx.r6.u32, rect = ctx.r5.u32;
    uint32_t f1 = tex ? LoadBE32(tex + kTextureFetch + 4) : 0, f2 = tex ? LoadBE32(tex + kTextureFetch + 8) : 0;
    CAPTURE_LOG("resolve flags %X rect %s(%d,%d,%d,%d) dest %08X fmt %u base %08X %ux%u surface %08X color %08X",
                ctx.r4.u32, rect ? "" : "none", rect ? int(LoadBE32(rect)) : 0, rect ? int(LoadBE32(rect + 4)) : 0,
                rect ? int(LoadBE32(rect + 8)) : 0, rect ? int(LoadBE32(rect + 12)) : 0, tex, f1 & 0x3F,
                f1 & 0xFFFFF000, (f2 & 0x1FFF) + 1, ((f2 >> 13) & 0x1FFF) + 1, LoadBE32(device + 0x2880),
                LoadBE32(device + 0x2884));
  }
  __imp__sub_826D9588(ctx, base);
}

REX_HOOK_RAW(sub_826D6B98) {
  // Clear(device, count, rects, flags, color, z, stencil): bit 0 of flags = color.
  if (Enabled() && (ctx.r6.u32 & 1)) {
    RaymanNativeRendererClear(g_rayman_membase + ctx.r3.u32 + native::kStateBegin, ctx.r7.u32);
  }
  if (std::getenv("RAYMAN_NATIVE_CAPTURE") && g_frame % 120 == 0) {
    uint32_t device = ctx.r3.u32;
    CAPTURE_LOG("clear count %u flags %X color %08X z %g surface %08X color %08X", ctx.r4.u32, ctx.r6.u32,
                ctx.r7.u32, ctx.f1.f64, LoadBE32(device + 0x2880), LoadBE32(device + 0x2884));
  }
  __imp__sub_826D6B98(ctx, base);
}
