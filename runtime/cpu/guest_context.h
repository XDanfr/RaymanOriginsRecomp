#pragma once
#include "ppc_recomp_shared.h"

// Contexto de CPU do guest da thread atual. Imports que chamam de volta código
// do guest (GuestToHostFunction) precisam dele. Igual ao cpu/ppc_context.h do
// Unleashed Recompiled.
inline thread_local PPCContext* g_ppcContext;

inline PPCContext* GetPPCContext()
{
    return g_ppcContext;
}

inline void SetPPCContext(PPCContext& ctx)
{
    g_ppcContext = &ctx;
}

inline void ClearPPCContext()
{
    g_ppcContext = nullptr;
}
