#include "guest_bootstrap.h"

#include <cstdio>
#include <pthread.h>

#include "cpu/guest_context.h"
#include "kernel/thread.h"

namespace
{
constexpr size_t GUEST_BOOTSTRAP_HOST_STACK = 1 * 1024 * 1024;

struct BootstrapState
{
    GuestBootstrapResult result;
    bool contextReady = false;
};

void* GuestBootstrapMain(void* argument)
{
    auto& state = *static_cast<BootstrapState*>(argument);
    PPCContext context;
    const auto guestThread = InitMainThread(context);
    if (guestThread == nullptr)
        return nullptr;

    state.result.pcr = context.r13.u32;
    state.result.guestStackTop = context.r1.u32;
    state.contextReady = state.result.pcr == guestThread->block &&
                         state.result.guestStackTop > state.result.pcr;
    ClearPPCContext();
    return nullptr;
}
}

bool StartGuestBootstrap(GuestBootstrapResult& result)
{
    result = {};
    BootstrapState state;

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    const int stackResult = pthread_attr_setstacksize(
        &attributes, GUEST_BOOTSTRAP_HOST_STACK);

    pthread_t thread;
    const int createResult = stackResult == 0
        ? pthread_create(&thread, &attributes, GuestBootstrapMain, &state)
        : stackResult;
    pthread_attr_destroy(&attributes);

    if (createResult != 0)
    {
        fprintf(stderr, "[switch] guest bootstrap pthread creation failed (%d)\n", createResult);
        return false;
    }

    pthread_join(thread, nullptr);
    if (!state.contextReady)
    {
        fprintf(stderr, "[switch] guest bootstrap context setup failed\n");
        return false;
    }

    result = state.result;
    return true;
}
