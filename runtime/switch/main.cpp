#include "host/platform.h"

// libnx's runtime pad helper uses the same global type name as the
// platform-neutral runtime input state.
#define PadState LibnxPadState
#include <switch.h>
#undef PadState

#include <cstdio>
#include "memory.h"

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

int main()
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
    printf("Return to the HOME menu to exit.\n");
    consoleUpdate(nullptr);

    WaitForExit();
    consoleExit(nullptr);
    return 0;
}
