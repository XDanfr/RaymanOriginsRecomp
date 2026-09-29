#pragma once

// Forces the strong D3D wrappers in native_hooks.cpp into the final link.
// Generated D3D entry points are weak, so these wrappers can observe the real
// high-level draw stream while continuing into the original recompiled code.
void InitNativeGraphicsHooks();
