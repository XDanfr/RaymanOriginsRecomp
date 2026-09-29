#include "host/platform.h"

// libnx's runtime pad helper uses the same global type name as the
// platform-neutral runtime input state.
#define PadState LibnxPadState
#include <switch.h>
#undef PadState

#include <array>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <string>
#include "apu/audio.h"
#include "cpu/guest_context.h"
#include "gpu/native_hooks.h"
#include "loader.h"
#include "memory.h"
#include "kernel/memory_layout.h"
#include "switch/guest_bootstrap.h"
#include "switch/presenter.h"

namespace
{
constexpr const char* RUNTIME_LOG_PATH =
    "sdmc:/switch/RaymanOriginsRecomp/runtime.log";
constexpr const char* CRASH_LOG_PATH =
    "sdmc:/switch/RaymanOriginsRecomp/crash.log";

bool InitRuntimeLog()
{
    // A successful run must not leave a previous failure looking current.
    std::remove(CRASH_LOG_PATH);
    FILE* redirected = freopen(RUNTIME_LOG_PATH, "w", stderr);
    if (redirected == nullptr)
        return false;

    setvbuf(stderr, nullptr, _IOLBF, BUFSIZ);
    fprintf(stderr, "Rayman Origins Recompiled Switch runtime log\n");
    return true;
}

[[noreturn]] void HandleUnexpectedTerminate()
{
    FILE* file = fopen(CRASH_LOG_PATH, "w");
    if (file != nullptr)
    {
        fprintf(file, "Rayman Origins Recompiled Switch termination\n");
        fprintf(file, "reason=uncaught C++ exception or std::terminate\n");
        if (PPCContext* context = GetPPCContext())
        {
            fprintf(file, "guest_lr=0x%08X guest_r1=0x%08X guest_r13=0x%08X\n",
                    static_cast<uint32_t>(context->lr), context->r1.u32, context->r13.u32);
            fprintf(file, "guest_r3=0x%08X guest_r4=0x%08X guest_r5=0x%08X\n",
                    context->r3.u32, context->r4.u32, context->r5.u32);
        }
        fclose(file);
    }
    fflush(stderr);
    std::_Exit(1);
}

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
    bool runningHeartbeatReported = false;
    uint32_t loopCount = 0;
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

            if (!runningHeartbeatReported && ++loopCount >= 200 &&
                GetGuestExecutionState() == GuestExecutionState::Running)
            {
                printf("\nGuest is still running after 10 seconds.\n");
                printf("Runtime diagnostics: %s\n", RUNTIME_LOG_PATH);
                fprintf(stderr, "[switch] guest still running after 10 seconds\n");
                runningHeartbeatReported = true;
            }
        }
        PumpSwitchPresentation();
        if (!SwitchPresentationOwnsDisplay())
            consoleUpdate(nullptr);
        svcSleepThread(50'000'000);
    }
}
}

int main(int argc, char** argv)
{
    std::set_terminate(HandleUnexpectedTerminate);
    consoleInit(nullptr);
    const bool runtimeLogReady = InitRuntimeLog();
    InitPlatform();

    printf("Rayman Origins Recompiled\n");
    printf("Switch bootstrap OK\n");
    printf("libnx + devkitA64 executable started successfully.\n\n");
    if (!runtimeLogReady)
        printf("Warning: could not create %s\n\n", RUNTIME_LOG_PATH);

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

    if (!InitAudioHardware())
    {
        printf("XMA audio MMIO initialisation FAILED.\n");
        consoleUpdate(nullptr);
        WaitForExit();
        consoleExit(nullptr);
        return 1;
    }

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
    printf("The console will remain active until the first guest frame.\n");
    printf("Then the presentation probe should show animated colour bars.\n\n");
    printf("Return to the HOME menu to exit.\n");
    consoleUpdate(nullptr);
    svcSleepThread(100'000'000);

    InitNativeGraphicsHooks();
    RunGuestEntry();
    WaitForExit(true);
    const bool presentationOwnedDisplay = SwitchPresentationOwnsDisplay();
    ShutdownSwitchPresentation();
    if (!presentationOwnedDisplay)
        consoleExit(nullptr);
    return 0;
}
