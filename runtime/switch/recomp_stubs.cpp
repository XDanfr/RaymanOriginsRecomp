#include <cstdio>
#include <cstdlib>
#include "cpu/guest_context.h"

namespace
{
struct MissingIndirectCall
{
};
}

[[noreturn]] void PpcMissingIndirectCall(uint32_t target, PPCContext& ctx)
{
    std::fprintf(stderr,
        "[recomp] missing indirect call target=0x%08X lr=0x%08X r1=0x%08X\n",
        target,
        static_cast<uint32_t>(ctx.lr),
        ctx.r1.u32);

    std::printf("[recomp] missing call 0x%08X from lr=0x%08X\n",
        target, static_cast<uint32_t>(ctx.lr));
    throw MissingIndirectCall{};
}
