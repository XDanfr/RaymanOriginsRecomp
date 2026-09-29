#pragma once

#include <cstdint>

struct PPCContext;

// The real Switch renderer is compiled only when a complete external NVK SDK
// is supplied.  These entry points remain harmless stubs in bootstrap builds.
bool InitSwitchNativeRenderer();
bool SwitchNativeRendererActive();
void RegisterSwitchNativeShader(uint32_t object, uint64_t hash);
void SubmitSwitchNativeDraw(uint32_t entry, const PPCContext& context);
void SubmitSwitchNativeClear(const PPCContext& context);
void SubmitSwitchNativeResolve(const PPCContext& context);
void PresentSwitchNativeFrame();
