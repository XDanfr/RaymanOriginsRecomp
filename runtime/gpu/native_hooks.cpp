#include "gpu/native_hooks.h"

#include "cpu/guest_context.h"
#include "memory.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

extern "C"
{
PPC_FUNC(__imp__sub_826CD850);
PPC_FUNC(__imp__sub_826CD668);
PPC_FUNC(__imp__sub_826D7588);
PPC_FUNC(__imp__sub_826D7170);
PPC_FUNC(__imp__sub_826D7128);
PPC_FUNC(__imp__sub_826D9588);
PPC_FUNC(__imp__sub_826D6B98);
PPC_FUNC(__imp__sub_826D41B8);
}

namespace
{
constexpr uint32_t DEVICE_PIXEL_SHADER = 0x330C;
constexpr uint32_t DEVICE_VERTEX_SHADER = 0x3310;

struct ShaderInfo
{
    uint64_t hash = 0;
    bool vertex = false;
};

struct DrawKey
{
    uint32_t entry = 0;
    uint32_t primitive = 0;
    uint64_t vertexShader = 0;
    uint64_t pixelShader = 0;

    bool operator<(const DrawKey& other) const
    {
        return std::tie(entry, primitive, vertexShader, pixelShader) <
               std::tie(other.entry, other.primitive, other.vertexShader, other.pixelShader);
    }
};

std::mutex g_mutex;
std::unordered_map<uint32_t, ShaderInfo> g_shaders;
std::map<DrawKey, uint32_t> g_draws;
uint64_t g_frames = 0;
uint64_t g_totalDraws = 0;
uint32_t g_frameDraws = 0;
uint32_t g_unknownShaderDraws = 0;
uint32_t g_clears = 0;
uint32_t g_resolves = 0;

uint32_t LoadBE32(uint32_t guest)
{
    uint32_t value = 0;
    std::memcpy(&value, g_memory.Translate(guest), sizeof(value));
    return __builtin_bswap32(value);
}

void RecordShader(uint32_t object, uint32_t function, bool vertex)
{
    if (object == 0 || function == 0)
        return;

    const uint32_t header = LoadBE32(function + 4);
    const uint32_t body = LoadBE32(function + 8);
    const uint64_t size64 = uint64_t(header) + uint64_t(body);
    if (size64 == 0 || size64 > (1u << 20) || function > UINT32_MAX - size64)
        return;

    const size_t size = static_cast<size_t>(size64);
    const uint64_t hash = XXH3_64bits(g_memory.Translate(function), size);
    {
        std::lock_guard lock(g_mutex);
        g_shaders[object] = { hash, vertex };
    }
    fprintf(stderr, "[native] %s shader 0x%08X hash %016llX\n",
            vertex ? "vertex" : "pixel", object,
            static_cast<unsigned long long>(hash));
}

uint64_t FindShaderHash(uint32_t object)
{
    const auto found = g_shaders.find(object);
    return found == g_shaders.end() ? 0 : found->second.hash;
}

void RecordDraw(uint32_t entry, const PPCContext& ctx)
{
    const uint32_t device = ctx.r3.u32;
    if (device == 0)
        return;

    const uint32_t vertexObject = LoadBE32(device + DEVICE_VERTEX_SHADER);
    const uint32_t pixelObject = LoadBE32(device + DEVICE_PIXEL_SHADER);

    std::lock_guard lock(g_mutex);
    const uint64_t vertexHash = FindShaderHash(vertexObject);
    const uint64_t pixelHash = FindShaderHash(pixelObject);
    g_draws[{ entry, ctx.r4.u32, vertexHash, pixelHash }]++;
    g_frameDraws++;
    g_totalDraws++;
    if ((vertexObject != 0 && vertexHash == 0) || (pixelObject != 0 && pixelHash == 0))
        g_unknownShaderDraws++;
}

void FinishFrame()
{
    std::lock_guard lock(g_mutex);
    g_frames++;
    if (g_frames <= 3 || (g_frames % 300) == 0)
    {
        fprintf(stderr,
                "[native] frame %llu: %u draws, %zu combinações, %u shaders desconhecidos, "
                "%zu shaders, %u clears, %u resolves (%llu draws total)\n",
                static_cast<unsigned long long>(g_frames), g_frameDraws, g_draws.size(),
                g_unknownShaderDraws, g_shaders.size(), g_clears, g_resolves,
                static_cast<unsigned long long>(g_totalDraws));
    }
    g_draws.clear();
    g_frameDraws = 0;
    g_unknownShaderDraws = 0;
    g_clears = 0;
    g_resolves = 0;
}
}

void InitNativeGraphicsHooks()
{
    fprintf(stderr,
            "[native] hooks D3D instalados: shaders, draw, clear, resolve e present\n");
}

PPC_FUNC(sub_826CD850)
{
    const uint32_t function = ctx.r3.u32;
    __imp__sub_826CD850(ctx, base);
    RecordShader(ctx.r3.u32, function, true);
}

PPC_FUNC(sub_826CD668)
{
    const uint32_t function = ctx.r3.u32;
    __imp__sub_826CD668(ctx, base);
    RecordShader(ctx.r3.u32, function, false);
}

PPC_FUNC(sub_826D7588)
{
    RecordDraw(0, ctx);
    __imp__sub_826D7588(ctx, base);
}

PPC_FUNC(sub_826D7170)
{
    RecordDraw(1, ctx);
    __imp__sub_826D7170(ctx, base);
}

PPC_FUNC(sub_826D7128)
{
    RecordDraw(2, ctx);
    __imp__sub_826D7128(ctx, base);
}

PPC_FUNC(sub_826D9588)
{
    {
        std::lock_guard lock(g_mutex);
        g_resolves++;
    }
    __imp__sub_826D9588(ctx, base);
}

PPC_FUNC(sub_826D6B98)
{
    {
        std::lock_guard lock(g_mutex);
        g_clears++;
    }
    __imp__sub_826D6B98(ctx, base);
}

PPC_FUNC(sub_826D41B8)
{
    __imp__sub_826D41B8(ctx, base);
    FinishFrame();
}
