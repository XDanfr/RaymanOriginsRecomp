#pragma once
#include <cstdint>

// Destino dos frames de áudio do jogo: 256 amostras estéreo intercaladas (float).
using AudioSink = void (*)(const float* stereo, uint32_t frames);

// Commit the Xbox 360 XMA register window used directly by translated code.
// The decoder semantics are still stubbed, but the guest must be able to
// perform the hardware lock/kick register accesses during audio startup.
bool InitAudioHardware();
void SetAudioSink(AudioSink sink);
