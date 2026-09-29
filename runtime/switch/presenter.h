#pragma once

#include <cstdint>

// Called by the GPU worker when the guest submits XE_SWAP. The main libnx
// thread consumes only the newest frame, so a slow diagnostic presenter never
// stalls the guest command processor.
void SubmitSwitchPresentationFrame(
    uint64_t frame,
    uint32_t frontBuffer,
    uint32_t width,
    uint32_t height);

// Called from the libnx main loop. On the first guest frame this releases the
// text console and takes ownership of the default NWindow with a framebuffer.
void PumpSwitchPresentation();
void ShutdownSwitchPresentation();
bool SwitchPresentationOwnsDisplay();
