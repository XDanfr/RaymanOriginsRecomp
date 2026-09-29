#include "host/platform.h"

// libnx's runtime pad helper uses the same global type name as the
// platform-neutral runtime input state.
#define PadState LibnxPadState
#include <switch.h>
#undef PadState

#include <array>
#include <cstdio>
#include <string>
#include "loader.h"
#include "memory.h"
#include "kernel/memory_layout.h"
#include "switch/guest_bootstrap.h"

namespace
{
bool ValidateGameData(const char* xexPath)
{
    const std::string path = xexPath;
    const size_t separator = path.find_last_of("/\\");
    const std::string root = separator == std::string::npos
        ? std::string(".")
        : path.substr(0, separator);

    constexpr std::array<const char*, 4> requiredFiles = {
        "localisation/localisation.loc",
        "secure_fat.gf",
        "bootsequence_X360.ipk",
        "menus_X360.ipk",
    };

    bool complete = true;
    for (const char* relativePath : requiredFiles)
    {
        const std::string fullPath = root + "/" + relativePath;
        FILE* file = fopen(fullPath.c_str(), "rb");
        if (file != nullptr)
        {
            fclose(file);
            continue;
        }

        printf("Missing: %s\n", relativePath);
        complete = false;
    }

    if (!complete)
    {
        printf("\nGame data is incomplete.\n");
        printf("Copy the complete contents of your own extracted Xbox 360 disc to:\n");
        printf("%s\n", root.c_str());
    }
    return complete;
}

void WaitForExit(bool reportGuestState = false)
{
    bool finalStateReported = false;
    while (appletMainLoop())
    {
        if (reportGuestState && !finalStateReported)
        {
            switch (GetGuestExecutionState())
            {
            case GuestExecutionState::Returned:
                printf("\nGuest entry point returned: r3=0x%08X\n",
                       GetGuestExecutionResult());
                finalStateReported = true;
                break;
            case GuestExecutionState::RequestedExit:
                printf("\nGuest requested exit: code=0x%08X\n",
                       GetGuestExecutionResult());
                finalStateReported = true;
                break;
            case GuestExecutionState::StoppedByException:
                printf("\nGuest execution stopped by a runtime exception.\n");
                printf("See the last diagnostic line above.\n");
                finalStateReported = true;
                break;
            default:
                break;
            }
        }
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

    if (!ValidateGameData(xexPath))
    {
        consoleUpdate(nullptr);
        WaitForExit();
        consoleExit(nullptr);
        return 1;
    }

    printf("Essential game data found.\n");
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
    if (!PrepareGuestEntry(image.entryPoint, bootstrap))
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
    printf("Calling generated entry point 0x%08X...\n", image.entryPoint);
    printf("The console will remain active for diagnostics.\n\n");
    printf("Return to the HOME menu to exit.\n");
    consoleUpdate(nullptr);
    svcSleepThread(100'000'000);

    RunGuestEntry();
    WaitForExit(true);
    consoleExit(nullptr);
    return 0;
}
