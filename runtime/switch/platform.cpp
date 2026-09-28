#include "host/platform.h"

// libnx also declares a global PadState.  Rename that declaration while
// importing the header so it can coexist with the runtime's platform-neutral
// PadState ABI.
#define PadState LibnxPadState
#include <switch.h>
#undef PadState

#include <algorithm>
#include <mutex>
#include <xbox.h>

namespace
{
std::once_flag g_padInit;
std::mutex g_padMutex;
LibnxPadState g_pad{};
bool g_padConnected = false;

void EnsurePadInitialised()
{
    std::call_once(g_padInit, [] { padInitializeDefault(&g_pad); });
}

int16_t ToXboxAxis(int32_t value)
{
    return static_cast<int16_t>(std::clamp(value, -32768, 32767));
}

void SetIfPressed(PadState& destination, uint64_t buttons, uint64_t source, uint16_t target)
{
    if (buttons & source)
        destination.buttons |= target;
}
}

bool InitPlatform()
{
    EnsurePadInitialised();
    return true;
}

void RunPlatformLoop()
{
    // The Switch bootstrap owns appletMainLoop in runtime/switch/main.cpp.
}

void ShutdownPlatform()
{
}

PadState GetPadState()
{
    EnsurePadInitialised();
    std::lock_guard lock(g_padMutex);
    padUpdate(&g_pad);
    g_padConnected = padIsConnected(&g_pad);

    PadState result;
    uint64_t buttons = padGetButtons(&g_pad);
    SetIfPressed(result, buttons, HidNpadButton_Up, XAMINPUT_GAMEPAD_DPAD_UP);
    SetIfPressed(result, buttons, HidNpadButton_Down, XAMINPUT_GAMEPAD_DPAD_DOWN);
    SetIfPressed(result, buttons, HidNpadButton_Left, XAMINPUT_GAMEPAD_DPAD_LEFT);
    SetIfPressed(result, buttons, HidNpadButton_Right, XAMINPUT_GAMEPAD_DPAD_RIGHT);
    SetIfPressed(result, buttons, HidNpadButton_Plus, XAMINPUT_GAMEPAD_START);
    SetIfPressed(result, buttons, HidNpadButton_Minus, XAMINPUT_GAMEPAD_BACK);
    SetIfPressed(result, buttons, HidNpadButton_StickL, XAMINPUT_GAMEPAD_LEFT_THUMB);
    SetIfPressed(result, buttons, HidNpadButton_StickR, XAMINPUT_GAMEPAD_RIGHT_THUMB);
    SetIfPressed(result, buttons, HidNpadButton_L, XAMINPUT_GAMEPAD_LEFT_SHOULDER);
    SetIfPressed(result, buttons, HidNpadButton_R, XAMINPUT_GAMEPAD_RIGHT_SHOULDER);
    SetIfPressed(result, buttons, HidNpadButton_A, XAMINPUT_GAMEPAD_A);
    SetIfPressed(result, buttons, HidNpadButton_B, XAMINPUT_GAMEPAD_B);
    SetIfPressed(result, buttons, HidNpadButton_X, XAMINPUT_GAMEPAD_X);
    SetIfPressed(result, buttons, HidNpadButton_Y, XAMINPUT_GAMEPAD_Y);
    result.leftTrigger = (buttons & HidNpadButton_ZL) ? 255 : 0;
    result.rightTrigger = (buttons & HidNpadButton_ZR) ? 255 : 0;

    HidAnalogStickState left = padGetStickPos(&g_pad, 0);
    HidAnalogStickState right = padGetStickPos(&g_pad, 1);
    result.thumbLX = ToXboxAxis(left.x);
    result.thumbLY = ToXboxAxis(left.y);
    result.thumbRX = ToXboxAxis(right.x);
    result.thumbRY = ToXboxAxis(right.y);
    return result;
}

bool IsPadConnected(uint32_t userIndex)
{
    if (userIndex != 0)
        return false;

    GetPadState();
    std::lock_guard lock(g_padMutex);
    return g_padConnected;
}

void SetPadVibration(uint16_t left, uint16_t right)
{
    // Vibration needs a controller-style-specific libnx setup.  Keep input
    // fully functional during bootstrap and leave haptics disabled for now.
    (void)left;
    (void)right;
}
