#pragma once

#include <cstdint>

// Result of creating a host pthread and preparing its PPC guest context. The
// bootstrap deliberately does not enter generated game code.
struct GuestBootstrapResult
{
    uint32_t pcr = 0;
    uint32_t guestStackTop = 0;
};

bool StartGuestBootstrap(GuestBootstrapResult& result);
