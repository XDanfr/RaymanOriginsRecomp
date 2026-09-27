#pragma once
// Included forcibly (-include) in every recompiled translation unit, before
// ppc_context.h. This header also carries the small compiler compatibility
// layer needed when generated XenonRecomp code is compiled with devkitA64 GCC.
#include <bit>
#include <cstdint>

#if defined(__GNUC__) && !defined(__clang__)

// XenonRecomp emits Clang's rotate builtins. devkitA64 uses GCC, so provide
// equivalent C++20 operations with the same fixed-width rotate semantics.
#define __builtin_rotateleft32(x, y) \
    std::rotl(static_cast<uint32_t>(x), static_cast<int>(y))

#define __builtin_rotateleft64(x, y) \
    std::rotl(static_cast<uint64_t>(x), static_cast<int>(y))

// XenonRecomp uses this only as an optimizer hint that the guest base is
// 32-byte aligned. GCC has no equivalent boolean builtin, so dropping the
// hint preserves correctness without changing generated code semantics.
#define __builtin_assume(x) ((void)0)

#endif

struct PPCContext;
[[noreturn]] void PpcMissingIndirectCall(uint32_t target, PPCContext& ctx);

#define PPC_CALL_INDIRECT_FUNC(x)                                   \
    do                                                              \
    {                                                               \
        uint32_t ppcTarget_ = (x);                                  \
        PPCFunc* ppcFunc_ = PPC_LOOKUP_FUNC(base, ppcTarget_);      \
        if (__builtin_expect(ppcFunc_ == nullptr, 0))               \
            PpcMissingIndirectCall(ppcTarget_, ctx);                \
        ppcFunc_(ctx, base);                                        \
    } while (0)
