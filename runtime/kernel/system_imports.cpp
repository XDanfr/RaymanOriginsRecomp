// Imports de sistema: módulos Xex, configuração do console, tempo, strings Rtl,
// mensagens/notificações e erros fatais. ExGetXConfigSetting, XMsgInProcessCall e
// as conversões de string seguem o Unleashed Recompiled (GPL-3.0).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "function.h"
#include "heap.h"
#include "objects.h"
#include "xbox_defs.h"

constexpr uint32_t STATUS_DLL_NOT_FOUND = 0xC0000135;
constexpr uint32_t STATUS_PROCEDURE_NOT_FOUND = 0xC000007A;
constexpr uint32_t STATUS_BUFFER_OVERFLOW = 0x80000005;

// Handles fictícios de módulo (o jogo só compara/passa adiante).
constexpr uint32_t MODULE_EXECUTABLE = 0x00001001;
constexpr uint32_t MODULE_XBOXKRNL = 0x00001002;
constexpr uint32_t MODULE_XAM = 0x00001003;

// Idioma do console (XC_LANGUAGE_*): 1 = inglês, 5 = espanhol, 9 = português...
static uint32_t ConsoleLanguage()
{
    static uint32_t language = [] {
        const char* env = getenv("RAYMAN_LANGUAGE");
        return env ? uint32_t(atoi(env)) : 1u;
    }();
    return language;
}

// ---- Módulos ----

static uint32_t XexGetModuleHandle(const char* name, be<uint32_t>* handle)
{
    uint32_t result = 0;
    if (name == nullptr)
        result = MODULE_EXECUTABLE;
    else if (!strcasecmp(name, "xboxkrnl.exe"))
        result = MODULE_XBOXKRNL;
    else if (!strcasecmp(name, "xam.xex"))
        result = MODULE_XAM;
    else if (!strcasecmp(name, "default.xex"))
        result = MODULE_EXECUTABLE;

    fprintf(stderr, "[xex] XexGetModuleHandle(\"%s\") -> 0x%X\n", name ? name : "(null)", result);
    if (handle)
        handle->set(result);
    return result ? X_STATUS_SUCCESS : STATUS_DLL_NOT_FOUND;
}

static uint32_t XexGetProcedureAddress(uint32_t module, uint32_t ordinal, be<uint32_t>* address)
{
    // Não há endereço do guest para exports do kernel resolvidos em runtime.
    // Registrar para implementar sob demanda os que o jogo realmente usar.
    fprintf(stderr, "[xex] XexGetProcedureAddress(módulo=0x%X, ordinal=%u / 0x%X) não suportado\n", module, ordinal, ordinal);
    if (address)
        address->set(0);
    return STATUS_PROCEDURE_NOT_FOUND;
}

static uint32_t XexCheckExecutablePrivilege(uint32_t privilege)
{
    (void)privilege;
    return 0;
}

static uint32_t XexLoadImage(const char* name, uint32_t flags, uint32_t minVersion, be<uint32_t>* handle)
{
    fprintf(stderr, "[xex] XexLoadImage(\"%s\") não suportado\n", name ? name : "(null)");
    (void)flags; (void)minVersion; (void)handle;
    return STATUS_DLL_NOT_FOUND;
}

static uint32_t XexUnloadImage(uint32_t handle)
{
    (void)handle;
    return X_STATUS_SUCCESS;
}

static uint32_t RtlImageXexHeaderField(uint32_t headerBase, uint32_t key)
{
    fprintf(stderr, "[xex] RtlImageXexHeaderField(0x%08X, 0x%08X) não suportado\n", headerBase, key);
    return 0;
}

// ---- Processo e console ----

static uint32_t KeGetCurrentProcessType() { return 1; } // X_PROCTYPE_USER
static void KeSetCurrentProcessType(uint32_t type) { (void)type; }
static void KeEnableFpuExceptions(uint32_t enabled) { (void)enabled; }
static uint32_t XGetLanguage() { return ConsoleLanguage(); }
static uint32_t XGetAVPack() { return 0; }
static uint32_t XGetGameRegion() { return 0x03FF; } // todas as regiões

static void FillVideoMode(XVIDEO_MODE* mode)
{
    memset(mode, 0, sizeof(*mode));
    mode->DisplayWidth = 1280;
    mode->DisplayHeight = 720;
    mode->IsInterlaced = false;
    mode->IsWidescreen = true;
    mode->IsHighDefinition = true;
    mode->RefreshRate = 0x42700000; // 60.0f
    mode->VideoStandard = 1;
    mode->Unknown4A = 0x4A;
    mode->Unknown01 = 0x01;
}

static void XGetVideoMode(XVIDEO_MODE* mode)
{
    FillVideoMode(mode);
}

static uint32_t ExGetXConfigSetting(uint16_t category, uint16_t setting, void* buffer, uint16_t bufferSize, be<uint32_t>* requiredSize)
{
    uint32_t data[4]{};
    switch (category)
    {
    case 0x0002: // XCONFIG_SECURED_CATEGORY
        if (setting != 0x0002) // XCONFIG_SECURED_AV_REGION
            return 1;
        data[0] = __builtin_bswap32(0x00001000); // EUA/Canadá
        break;
    case 0x0003: // XCONFIG_USER_CATEGORY
        switch (setting)
        {
        case 0x0001: case 0x0002: case 0x0003: case 0x0004:
        case 0x0005: case 0x0006: case 0x0007: // fuso horário
            data[0] = 0;
            break;
        case 0x0009: data[0] = __builtin_bswap32(ConsoleLanguage()); break; // idioma
        case 0x000A: data[0] = __builtin_bswap32(0x00040000); break;        // flags de vídeo
        case 0x000C: data[0] = __builtin_bswap32(1); break;                 // flags de varejo
        case 0x000E: data[0] = __builtin_bswap32(103); break;               // país
        default: return 1;
        }
        break;
    default:
        return 1;
    }
    if (requiredSize)
        requiredSize->set(4);
    if (buffer)
        memcpy(buffer, data, std::min<size_t>(bufferSize, sizeof(data)));
    return 0;
}

// ---- Tempo ----

constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;

static uint64_t KeQueryPerformanceFrequency()
{
    return 49875000; // timebase do Xbox 360; mftb é escalado para esta frequência
}

static void KeQuerySystemTime(be<uint64_t>* time)
{
    int64_t now = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    time->set(uint64_t(now + FILETIME_EPOCH_DIFFERENCE));
}

struct X_TIME_FIELDS
{
    be<uint16_t> year, month, day, hour, minute, second, milliseconds, weekday;
};

static void RtlTimeToTimeFields(be<int64_t>* time, X_TIME_FIELDS* fields)
{
    int64_t value = time->get();
    int64_t unixMs = (value - FILETIME_EPOCH_DIFFERENCE) / 10000;
    time_t seconds = time_t(unixMs / 1000);
    struct tm tm;
    gmtime_r(&seconds, &tm);
    fields->year = uint16_t(tm.tm_year + 1900);
    fields->month = uint16_t(tm.tm_mon + 1);
    fields->day = uint16_t(tm.tm_mday);
    fields->hour = uint16_t(tm.tm_hour);
    fields->minute = uint16_t(tm.tm_min);
    fields->second = uint16_t(tm.tm_sec);
    fields->milliseconds = uint16_t(unixMs % 1000);
    fields->weekday = uint16_t(tm.tm_wday);
}

// Convert a proleptic Gregorian calendar date to Unix days without relying on
// the host's timezone database or the non-standard timegm() extension.
static int64_t DaysFromCivil(int64_t year, uint32_t month, uint32_t day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const uint32_t yearOfEra = uint32_t(year - era * 400);
    const uint32_t monthPrime = month > 2 ? month - 3 : month + 9;
    const uint32_t dayOfYear =
        (153 * monthPrime + 2) / 5 + day - 1;
    const uint32_t dayOfEra =
        yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + int64_t(dayOfEra) - 719468;
}

static uint32_t RtlTimeFieldsToTime(X_TIME_FIELDS* fields, be<int64_t>* time)
{
    const uint32_t month = fields->month.get();
    const uint32_t day = fields->day.get();
    const uint32_t hour = fields->hour.get();
    const uint32_t minute = fields->minute.get();
    const uint32_t second = fields->second.get();
    const uint32_t milliseconds = fields->milliseconds.get();

    if (month < 1 || month > 12 ||
        day < 1 || day > 31 ||
        hour > 23 || minute > 59 || second > 59 || milliseconds > 999)
        return 0;

    const int64_t unixSeconds =
        DaysFromCivil(fields->year.get(), month, day) * 86400 +
        int64_t(hour) * 3600 +
        int64_t(minute) * 60 +
        int64_t(second);

    time->set(unixSeconds * 10000000 +
              int64_t(milliseconds) * 10000 +
              FILETIME_EPOCH_DIFFERENCE);
    return 1;
}

// ---- Strings ----

struct X_UNICODE_STRING
{
    be<uint16_t> length;        // em bytes
    be<uint16_t> maximumLength; // em bytes
    be<uint32_t> buffer;
};

static void RtlInitAnsiString(XANSI_STRING* destination, char* source)
{
    const uint16_t length = source ? uint16_t(strlen(source)) : 0;
    destination->Length = length;
    destination->MaximumLength = source ? uint16_t(length + 1) : 0;
    destination->Buffer = source;
}

static void RtlInitUnicodeString(X_UNICODE_STRING* destination, be<uint16_t>* source)
{
    uint16_t length = 0;
    if (source)
        while (source[length].get() != 0)
            length++;
    destination->length = uint16_t(length * 2);
    destination->maximumLength = source ? uint16_t(length * 2 + 2) : 0;
    destination->buffer = source ? g_memory.MapVirtual(source) : 0;
}

static void RtlFreeAnsiString(XANSI_STRING* string)
{
    if (string->Buffer.ptr.get() != 0)
        g_runtimeHeap.Free(string->Buffer.get());
    string->Buffer = nullptr;
    string->Length = 0;
    string->MaximumLength = 0;
}

static uint32_t RtlUnicodeStringToAnsiString(XANSI_STRING* destination, X_UNICODE_STRING* source, uint32_t allocate)
{
    uint16_t chars = source->length.get() / 2;
    const be<uint16_t>* input = static_cast<const be<uint16_t>*>(g_memory.Translate(source->buffer.get()));

    char* output;
    if (allocate)
    {
        output = static_cast<char*>(g_runtimeHeap.Alloc(chars + 1));
        destination->Buffer = output;
        destination->MaximumLength = uint16_t(chars + 1);
    }
    else
    {
        output = destination->Buffer.get();
        if (chars + 1 > destination->MaximumLength.get())
            return STATUS_BUFFER_OVERFLOW;
    }

    for (uint16_t i = 0; i < chars; i++)
    {
        uint16_t c = input[i].get();
        output[i] = c < 256 ? char(c) : '?';
    }
    output[chars] = '\0';
    destination->Length = chars;
    return X_STATUS_SUCCESS;
}

static uint32_t RtlMultiByteToUnicodeN(be<uint16_t>* unicode, uint32_t maxBytesInUnicode, be<uint32_t>* bytesInUnicode,
                                       const char* multiByte, uint32_t bytesInMultiByte)
{
    uint32_t length = std::min(maxBytesInUnicode / 2, bytesInMultiByte);
    for (uint32_t i = 0; i < length; i++)
        unicode[i] = uint16_t(uint8_t(multiByte[i]));
    if (bytesInUnicode)
        bytesInUnicode->set(length * 2);
    return X_STATUS_SUCCESS;
}

static uint32_t RtlUnicodeToMultiByteN(char* multiByte, uint32_t maxBytesInMultiByte, be<uint32_t>* bytesInMultiByte,
                                       const be<uint16_t>* unicode, uint32_t bytesInUnicode)
{
    uint32_t length = std::min(bytesInUnicode / 2, maxBytesInMultiByte);
    for (uint32_t i = 0; i < length; i++)
    {
        uint16_t c = unicode[i].get();
        multiByte[i] = c < 256 ? char(c) : '?';
    }
    if (bytesInMultiByte)
        bytesInMultiByte->set(length);
    return X_STATUS_SUCCESS;
}

static uint32_t RtlNtStatusToDosError(uint32_t status)
{
    switch (status)
    {
    case X_STATUS_SUCCESS: return 0;
    case 0xC0000034: return 2;    // OBJECT_NAME_NOT_FOUND -> FILE_NOT_FOUND
    case 0xC000003A: return 3;    // OBJECT_PATH_NOT_FOUND -> PATH_NOT_FOUND
    case X_STATUS_ACCESS_DENIED: return 5;
    case STATUS_INVALID_HANDLE: return 6;
    case X_STATUS_NO_MEMORY: return 8;
    case 0xC0000011: return 38;   // END_OF_FILE -> HANDLE_EOF
    case X_STATUS_INVALID_PARAMETER: return 87;
    case STATUS_TIMEOUT: return 1460;
    default:
        return (status & 0xFFFF0000) == 0x80070000 ? (status & 0xFFFF) : 317; // ERROR_MR_MID_NOT_FOUND
    }
}

// Devolve quantos bytes (múltiplo de 4) são iguais ao padrão.
static uint32_t RtlCompareMemoryUlong(const be<uint32_t>* source, uint32_t length, uint32_t pattern)
{
    uint32_t count = 0;
    for (uint32_t i = 0; i < length / 4 && source[i].get() == pattern; i++)
        count += 4;
    return count;
}

// O padrão é gravado com os bytes na ordem do registrador (como o memset de 32 bits do console).
static void RtlFillMemoryUlong(be<uint32_t>* destination, uint32_t length, uint32_t pattern)
{
    for (uint32_t i = 0; i < length / 4; i++)
        destination[i] = pattern;
}

// ---- Mensagens e notificações ----

static uint32_t XMsgInProcessCall(uint32_t app, uint32_t message, be<uint32_t>* param1, be<uint32_t>* param2)
{
    if (message == 0x7001B && param1)
    {
        auto* ptr = static_cast<uint32_t*>(g_memory.Translate(param1[1].get()));
        ptr[0] = 0;
        ptr[1] = 0;
    }
    (void)app; (void)param2;
    return 0;
}

static uint32_t XMsgStartIORequest(uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer, uint32_t bufferSize)
{
    (void)app; (void)message; (void)overlapped; (void)buffer; (void)bufferSize;
    return X_STATUS_SUCCESS;
}

static uint32_t XMsgCancelIORequest(uint32_t overlapped, uint32_t wait)
{
    (void)overlapped; (void)wait;
    return X_STATUS_SUCCESS;
}

static uint32_t XNotifyGetNext(uint32_t handle, uint32_t match, be<uint32_t>* id, be<uint32_t>* param)
{
    (void)handle; (void)match; (void)id; (void)param;
    return 0; // nenhuma notificação pendente
}

static void XNotifyPositionUI(uint32_t position) { (void)position; }
static void ExRegisterTitleTerminateNotification(uint32_t registration, uint32_t create) { (void)registration; (void)create; }
static uint32_t FscSetCacheElementCount(uint32_t unknown, uint32_t count) { (void)unknown; (void)count; return 0; }

// ---- Erros fatais e exceções ----

static void KeBugCheckEx(uint32_t code, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4)
{
    fprintf(stderr, "[kernel] KeBugCheckEx(0x%X, 0x%X, 0x%X, 0x%X, 0x%X)\n", code, p1, p2, p3, p4);
    abort();
}

static void KeBugCheck(uint32_t code)
{
    KeBugCheckEx(code, 0, 0, 0, 0);
}

static void HalReturnToFirmware(uint32_t routine)
{
    fprintf(stderr, "[kernel] HalReturnToFirmware(%u): o jogo pediu para sair\n", routine);
    exit(0);
}

static void DbgBreakPoint()
{
    fprintf(stderr, "[kernel] DbgBreakPoint\n");
}

static void RtlRaiseException(uint32_t record)
{
    uint32_t code = record ? static_cast<be<uint32_t>*>(g_memory.Translate(record))->get() : 0;
    fprintf(stderr, "[kernel] RtlRaiseException(código 0x%08X): exceções do guest não são suportadas\n", code);
    abort();
}

static uint32_t __C_specific_handler() { return 1; } // ExceptionContinueSearch
static void RtlUnwind() { fprintf(stderr, "[kernel] RtlUnwind não suportado\n"); }

GUEST_FUNCTION_HOOK(__imp__XexGetModuleHandle, XexGetModuleHandle);
GUEST_FUNCTION_HOOK(__imp__XexGetProcedureAddress, XexGetProcedureAddress);
GUEST_FUNCTION_HOOK(__imp__XexCheckExecutablePrivilege, XexCheckExecutablePrivilege);
GUEST_FUNCTION_HOOK(__imp__XexLoadImage, XexLoadImage);
GUEST_FUNCTION_HOOK(__imp__XexUnloadImage, XexUnloadImage);
GUEST_FUNCTION_HOOK(__imp__RtlImageXexHeaderField, RtlImageXexHeaderField);
GUEST_FUNCTION_HOOK(__imp__KeGetCurrentProcessType, KeGetCurrentProcessType);
GUEST_FUNCTION_HOOK(__imp__KeSetCurrentProcessType, KeSetCurrentProcessType);
GUEST_FUNCTION_HOOK(__imp__KeEnableFpuExceptions, KeEnableFpuExceptions);
GUEST_FUNCTION_HOOK(__imp__XGetLanguage, XGetLanguage);
GUEST_FUNCTION_HOOK(__imp__XGetAVPack, XGetAVPack);
GUEST_FUNCTION_HOOK(__imp__XGetGameRegion, XGetGameRegion);
GUEST_FUNCTION_HOOK(__imp__XGetVideoMode, XGetVideoMode);
GUEST_FUNCTION_HOOK(__imp__ExGetXConfigSetting, ExGetXConfigSetting);
GUEST_FUNCTION_HOOK(__imp__KeQueryPerformanceFrequency, KeQueryPerformanceFrequency);
GUEST_FUNCTION_HOOK(__imp__KeQuerySystemTime, KeQuerySystemTime);
GUEST_FUNCTION_HOOK(__imp__RtlTimeToTimeFields, RtlTimeToTimeFields);
GUEST_FUNCTION_HOOK(__imp__RtlTimeFieldsToTime, RtlTimeFieldsToTime);
GUEST_FUNCTION_HOOK(__imp__RtlInitAnsiString, RtlInitAnsiString);
GUEST_FUNCTION_HOOK(__imp__RtlInitUnicodeString, RtlInitUnicodeString);
GUEST_FUNCTION_HOOK(__imp__RtlFreeAnsiString, RtlFreeAnsiString);
GUEST_FUNCTION_HOOK(__imp__RtlUnicodeStringToAnsiString, RtlUnicodeStringToAnsiString);
GUEST_FUNCTION_HOOK(__imp__RtlMultiByteToUnicodeN, RtlMultiByteToUnicodeN);
GUEST_FUNCTION_HOOK(__imp__RtlUnicodeToMultiByteN, RtlUnicodeToMultiByteN);
GUEST_FUNCTION_HOOK(__imp__RtlNtStatusToDosError, RtlNtStatusToDosError);
GUEST_FUNCTION_HOOK(__imp__RtlCompareMemoryUlong, RtlCompareMemoryUlong);
GUEST_FUNCTION_HOOK(__imp__RtlFillMemoryUlong, RtlFillMemoryUlong);
GUEST_FUNCTION_HOOK(__imp__XMsgInProcessCall, XMsgInProcessCall);
GUEST_FUNCTION_HOOK(__imp__XMsgStartIORequest, XMsgStartIORequest);
GUEST_FUNCTION_HOOK(__imp__XMsgCancelIORequest, XMsgCancelIORequest);
GUEST_FUNCTION_HOOK(__imp__XNotifyGetNext, XNotifyGetNext);
GUEST_FUNCTION_HOOK(__imp__XNotifyPositionUI, XNotifyPositionUI);
GUEST_FUNCTION_HOOK(__imp__ExRegisterTitleTerminateNotification, ExRegisterTitleTerminateNotification);
GUEST_FUNCTION_HOOK(__imp__FscSetCacheElementCount, FscSetCacheElementCount);
GUEST_FUNCTION_HOOK(__imp__KeBugCheckEx, KeBugCheckEx);
GUEST_FUNCTION_HOOK(__imp__KeBugCheck, KeBugCheck);
GUEST_FUNCTION_HOOK(__imp__HalReturnToFirmware, HalReturnToFirmware);
GUEST_FUNCTION_HOOK(__imp__DbgBreakPoint, DbgBreakPoint);
GUEST_FUNCTION_HOOK(__imp__RtlRaiseException, RtlRaiseException);
GUEST_FUNCTION_HOOK(__imp____C_specific_handler, __C_specific_handler);
GUEST_FUNCTION_HOOK(__imp__RtlUnwind, RtlUnwind);
