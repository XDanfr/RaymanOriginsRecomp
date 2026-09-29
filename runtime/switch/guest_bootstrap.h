#pragma once

#include <cstdint>

// Result of creating a host pthread and preparing its PPC guest context. The
// bootstrap deliberately does not enter generated game code.
struct GuestBootstrapResult
{
    uint32_t pcr = 0;
    uint32_t guestStackTop = 0;
};

enum class GuestExecutionState : uint32_t
{
    NotStarted,
    Preparing,
    Prepared,
    Running,
    Returned,
    RequestedExit,
    StoppedByException,
    FailedToPrepare,
};

bool PrepareGuestEntry(uint32_t entryPoint, GuestBootstrapResult& result);
void RunGuestEntry();
GuestExecutionState GetGuestExecutionState();
uint32_t GetGuestExecutionResult();
