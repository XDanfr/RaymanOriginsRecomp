#include <switch.h>
#include <cstdio>

int main()
{
    consoleInit(nullptr);

    printf("Rayman Origins Recompiled\n");
    printf("Switch bootstrap OK\n");
    printf("libnx + devkitA64 executable started successfully.\n\n");
    printf("Press B to exit.\n");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    while (appletMainLoop())
    {
        hidScanInput();

        if (hidKeysDown(CONTROLLER_P1_AUTO) & HidNpadButton_B)
            break;

        consoleUpdate(nullptr);
        svcSleepThread(50'000'000);
    }

    consoleExit(nullptr);
    return 0;
}
