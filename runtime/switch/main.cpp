#include "host/platform.h"

// libnx's runtime pad helper uses the same global type name as the
// platform-neutral runtime input state.
#define PadState LibnxPadState
#include <switch.h>
#undef PadState

#include <cstdio>
#include "loader.h"
#include "memory.h"
#include "kernel/memory_layout.h"
#include "switch/guest_bootstrap.h"

namespace
{
void WaitForExit()
{
    while (appletMainLoop())
    {
        consoleUpdate(nullptr);
        svcSleepThread(50'000'000);
    }
}
}

int main(int argc, char** argv)
{
    consoleInit(nullptr);
    InitPlatform();

    printf("Rayman Origins Recompiled\n");
    printf("Switch bootstrap OK\n");
    printf("libnx + devkitA64 executable started successfully.\n\n");

    printf("Initialising 4 GB guest address window...\n");
    consoleUpdate(nullptr);

    if (!g_memory.Init())
    {
        printf("Guest memory initialisation FAILED.\n");
        printf("Check the Switch debug output for the Horizon mapping error.\n");
        consoleUpdate(nullptr);
        WaitForExit();
        consoleExit(nullptr);
        return 1;
    }

    constexpr uint32_t probeAddress = 0x00020000;
    constexpr uint32_t probeValue = 0x5241594Du;
    constexpr const char* defaultXexPath =
        "sdmc:/switch/RaymanOriginsRecomp/game/default.xex";

    printf("Guest window reserved. Testing a dynamic page commit...\n");
    consoleUpdate(nullptr);

    if (!g_memory.CommitRange(probeAddress, 0x1000))
    {
        printf("Guest memory commit FAILED.\n");
        consoleUpdate(nullptr);
        WaitForExit();
        consoleExit(nullptr);
        return 1;
    }

    volatile uint32_t* probe =
        static_cast<volatile uint32_t*>(g_memory.Translate(probeAddress));
    *probe = probeValue;

    if (*probe != probeValue)
    {
        printf("Guest memory read/write FAILED.\n");
        consoleUpdate(nullptr);
        WaitForExit();
        consoleExit(nullptr);
        return 1;
    }

    printf("Guest memory probe OK.\n");
    printf("4 GB sparse window + dynamic page mapping are working.\n\n");

    const char* xexPath = argc > 1 ? argv[1] : defaultXexPath;
    printf("Loading XEX image: %s\n", xexPath);
    consoleUpdate(nullptr);

    LoadedImage image;
    if (!LoadXexImage(xexPath, g_memory.base, image))
    {
        printf("XEX image load FAILED.\n");
        printf("Copy your own default.xex to:\n%s\n", defaultXexPath);
        consoleUpdate(nullptr);
        WaitForExit();
        consoleExit(nullptr);
        return 1;
    }

    if (g_memory.FindFunction(image.entryPoint) == nullptr)
    {
        printf("XEX entry point 0x%08X has no generated mapping.\n", image.entryPoint);
        consoleUpdate(nullptr);
        WaitForExit();
        consoleExit(nullptr);
        return 1;
    }

    printf("XEX image loaded: 0x%08X..0x%08X\n",
           image.base, image.base + image.size);
    printf("Generated entry mapping found: 0x%08X\n", image.entryPoint);
    printf("Initialising sparse guest runtime heap...\n");
    consoleUpdate(nullptr);

    if (!InitGuestHeaps())
    {
        printf("Guest runtime heap initialisation FAILED.\n");
        consoleUpdate(nullptr);
        WaitForExit();
        consoleExit(nullptr);
        return 1;
    }

    GuestBootstrapResult bootstrap;
    if (!StartGuestBootstrap(bootstrap))
    {
        printf("Guest thread context initialisation FAILED.\n");
        consoleUpdate(nullptr);
        WaitForExit();
        consoleExit(nullptr);
        return 1;
    }

    printf("Guest runtime heap and thread context OK.\n");
    printf("PCR: 0x%08X  Guest stack top: 0x%08X\n",
           bootstrap.pcr, bootstrap.guestStackTop);
    printf("Guest entry point is intentionally not called yet.\n\n");
    printf("Return to the HOME menu to exit.\n");
    consoleUpdate(nullptr);

    WaitForExit();
    consoleExit(nullptr);
    return 0;
}
