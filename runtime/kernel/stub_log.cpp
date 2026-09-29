#include "stub_log.h"
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include "ppc_recomp_shared.h"

void LogImportStub(const char* name, const PPCContext& ctx)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, uint64_t> calls;

    std::lock_guard lock(mutex);
    uint64_t n = ++calls[name];

    // As 8 primeiras chamadas sempre; depois só potências de 2 (para ver quem entra em loop).
    if (n > 8 && (n & (n - 1)) != 0)
        return;

    fprintf(stderr, "[stub] %-36s #%-6llu r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X\n",
            name, (unsigned long long)n,
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr);
#if defined(__SWITCH__)
    printf("[stub] %s #%llu lr=%08X\n",
           name, (unsigned long long)n, (uint32_t)ctx.lr);
#endif
}
