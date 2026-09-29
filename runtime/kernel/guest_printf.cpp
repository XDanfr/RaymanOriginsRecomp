// Imports da família printf. O formatador vem do Xenia (guest_printf_core.inl);
// os wrappers seguem o comportamento dos _entry do Xenia (xboxkrnl_strings.cc).
#include "guest_printf_core.inl"
#include <cstdio>
#include "function.h"

bool FormatGuestVarargs(PPCContext& ctx, const char* format, int32_t firstArg, std::string& out)
{
    StackArgList args(&ctx, firstArg);
    StringFormatData data(reinterpret_cast<const uint8_t*>(format));
    if (format_core(&ctx, data, args, false) < 0)
        return false;
    out = data.str();
    return true;
}

// Copia o resultado com a semântica de _snprintf: sem terminador se encher, -1 se transbordar.
static int32_t StoreBounded(char* buffer, int32_t bufferCount, int32_t count, const std::string& text)
{
    if (count < 0)
    {
        if (bufferCount > 0)
            buffer[0] = '\0';
        return count;
    }
    if (count <= bufferCount)
    {
        memcpy(buffer, text.c_str(), count);
        if (count < bufferCount)
            buffer[count] = '\0';
        return count;
    }
    memcpy(buffer, text.c_str(), bufferCount);
    return -1;
}

PPC_FUNC(__imp__sprintf)
{
    char* buffer = ctx.r3.u32 ? static_cast<char*>(g_memory.Translate(ctx.r3.u32)) : nullptr;
    const uint8_t* format = ctx.r4.u32 ? static_cast<const uint8_t*>(g_memory.Translate(ctx.r4.u32)) : nullptr;
    if (!buffer || !format)
    {
        ctx.r3.s64 = -1;
        return;
    }

    StackArgList args(&ctx, 2);
    StringFormatData data(format);
    int32_t count = format_core(&ctx, data, args, false);
    if (count <= 0)
        buffer[0] = '\0';
    else
    {
        memcpy(buffer, data.str().c_str(), count);
        buffer[count] = '\0';
    }
    ctx.r3.s64 = count;
}

PPC_FUNC(__imp___snprintf)
{
    char* buffer = ctx.r3.u32 ? static_cast<char*>(g_memory.Translate(ctx.r3.u32)) : nullptr;
    int32_t bufferCount = ctx.r4.s32;
    const uint8_t* format = ctx.r5.u32 ? static_cast<const uint8_t*>(g_memory.Translate(ctx.r5.u32)) : nullptr;
    if (!buffer || bufferCount <= 0 || !format)
    {
        ctx.r3.s64 = -1;
        return;
    }

    StackArgList args(&ctx, 3);
    StringFormatData data(format);
    int32_t count = format_core(&ctx, data, args, false);
    ctx.r3.s64 = StoreBounded(buffer, bufferCount, count, data.str());
}

PPC_FUNC(__imp___vsnprintf)
{
    char* buffer = ctx.r3.u32 ? static_cast<char*>(g_memory.Translate(ctx.r3.u32)) : nullptr;
    int32_t bufferCount = ctx.r4.s32;
    const uint8_t* format = ctx.r5.u32 ? static_cast<const uint8_t*>(g_memory.Translate(ctx.r5.u32)) : nullptr;
    uint32_t argPtr = ctx.r6.u32;
    if (!buffer || bufferCount <= 0 || !format)
    {
        ctx.r3.s64 = -1;
        return;
    }

    ArrayArgList args(&ctx, argPtr);
    StringFormatData data(format);
    int32_t count = format_core(&ctx, data, args, false);
    ctx.r3.s64 = StoreBounded(buffer, bufferCount, count, data.str());
}

PPC_FUNC(__imp__DbgPrint)
{
    std::string text;
    if (ctx.r3.u32 && FormatGuestVarargs(ctx, static_cast<const char*>(g_memory.Translate(ctx.r3.u32)), 1, text))
    {
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
        fprintf(stderr, "[DbgPrint] %s\n", text.c_str());
#if defined(__SWITCH__)
        printf("[DbgPrint] %s\n", text.c_str());
#endif
    }
    ctx.r3.u64 = 0;
}
