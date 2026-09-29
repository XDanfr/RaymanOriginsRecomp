// Imports do XAM: usuário/perfil, notificações, conteúdo (saves), enumeradores,
// diálogos do sistema, loader e input. Semântica do Xenia (xam_*.cc); partes
// seguem o Unleashed Recompiled (GPL-3.0).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include "file_system.h"
#include "host/platform.h"
#include <atomic>
#include "function.h"
#include "objects.h"
#include "thread.h"
#include "xbox_defs.h"

namespace fs = std::filesystem;

// Códigos de erro Win32 usados pelo XAM
constexpr uint32_t X_ERROR_SUCCESS = 0;
constexpr uint32_t X_ERROR_PATH_NOT_FOUND = 3;
constexpr uint32_t X_ERROR_NO_MORE_FILES = 18;
constexpr uint32_t X_ERROR_INSUFFICIENT_BUFFER = 122;
constexpr uint32_t X_ERROR_ALREADY_EXISTS = 183;
constexpr uint32_t X_ERROR_IO_PENDING = 997;
constexpr uint32_t X_ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr uint32_t X_ERROR_NO_SUCH_USER = 0x525;
constexpr uint32_t X_ERROR_FUNCTION_FAILED = 1627;

constexpr uint64_t LOCAL_XUID = 0xB13EBABEBABEBABEull;

// Conclui uma operação assíncrona na hora: preenche o XXOVERLAPPED, sinaliza o
// evento e devolve ERROR_IO_PENDING (como o Xenia). Sem overlapped, devolve o resultado.
static uint32_t Complete(XXOVERLAPPED* overlapped, uint32_t result, uint32_t length = 0)
{
    if (!overlapped)
        return result;
    overlapped->data.errorLength.Error = result;
    overlapped->data.errorLength.Length = length;
    overlapped->dwExtendedError = 0;
    if (uint32_t event = overlapped->hEvent.get())
    {
        if (auto object = GetObjectAs<Event>(event))
        {
            {
                auto lock = LockDispatcher();
                object->signaled = true;
                SyncHeaderSignalState(*object);
            }
            NotifyDispatcher();
        }
    }
    return X_ERROR_IO_PENDING;
}

// ---- Sistema e loader ----

static uint32_t XamGetSystemVersion() { return 0; }

static uint32_t XamGetExecutionId(be<uint32_t>* info)
{
    (void)info;
    return X_STATUS_UNSUCCESSFUL;
}

static void XamLoaderTerminateTitle()
{
    fprintf(stderr, "[xam] XamLoaderTerminateTitle: o jogo pediu para sair\n");
#if defined(__SWITCH__)
    printf("[xam] title requested termination\n");
    ExitCurrentThread(0);
#else
    exit(0);
#endif
}

static void XamLoaderLaunchTitle(const char* name, uint32_t flags)
{
    fprintf(stderr, "[xam] XamLoaderLaunchTitle(\"%s\", 0x%X): saindo\n", name ? name : "", flags);
#if defined(__SWITCH__)
    printf("[xam] title launch requested: %s\n", name ? name : "");
    ExitCurrentThread(0);
#else
    exit(0);
#endif
}

// ---- Usuário e perfil (um usuário local no índice 0) ----

static uint32_t XamUserGetSigninState(uint32_t userIndex)
{
    return userIndex == 0 ? 1 : 0; // 1 = logado localmente
}

static uint32_t XamUserGetSigninInfo(uint32_t userIndex, uint32_t flags, XUSER_SIGNIN_INFO* info)
{
    (void)flags;
    if (userIndex != 0)
        return X_ERROR_NO_SUCH_USER;
    memset(info, 0, sizeof(*info));
    info->xuid = LOCAL_XUID;
    info->SigninState = 1;
    strcpy(info->Name, "Rayman");
    return X_ERROR_SUCCESS;
}

static uint32_t XamUserGetXUID(uint32_t userIndex, uint32_t typeMask, be<uint64_t>* xuid)
{
    (void)typeMask;
    if (userIndex != 0)
        return X_ERROR_NO_SUCH_USER;
    if (xuid)
        xuid->set(LOCAL_XUID);
    return X_ERROR_SUCCESS;
}

// Layout do Xenia: cabeçalho {count, ptr} + 40 bytes por configuração.
struct X_USER_PROFILE_SETTING
{
    be<uint32_t> from;
    be<uint32_t> pad0;
    be<uint64_t> userIndexOrXuid;
    be<uint32_t> settingId;
    be<uint32_t> pad1;
    uint8_t data[16];
};
static_assert(sizeof(X_USER_PROFILE_SETTING) == 40);

static uint32_t XamUserReadProfileSettings(uint32_t titleId, uint32_t userIndex, uint32_t xuidCount, be<uint64_t>* xuids,
                                           uint32_t settingCount, be<uint32_t>* settingIds, be<uint32_t>* bufferSize,
                                           uint8_t* buffer, XXOVERLAPPED* overlapped)
{
    (void)titleId; (void)xuidCount; (void)xuids;
    uint32_t required = 8 + settingCount * uint32_t(sizeof(X_USER_PROFILE_SETTING));
    if (!buffer || bufferSize->get() < required)
    {
        bufferSize->set(required);
        return X_ERROR_INSUFFICIENT_BUFFER;
    }

    // Nenhuma configuração salva: o jogo usa os padrões dele.
    memset(buffer, 0, required);
    auto* header = reinterpret_cast<be<uint32_t>*>(buffer);
    header[0] = settingCount;
    header[1] = g_memory.MapVirtual(buffer + 8);
    auto* settings = reinterpret_cast<X_USER_PROFILE_SETTING*>(buffer + 8);
    for (uint32_t i = 0; i < settingCount; i++)
    {
        settings[i].from = 0;
        settings[i].userIndexOrXuid = userIndex;
        settings[i].settingId = settingIds[i].get();
    }
    return Complete(overlapped, X_ERROR_SUCCESS);
}

static uint32_t XamUserWriteProfileSettings(uint32_t titleId, uint32_t userIndex, uint32_t settingCount, void* settings, XXOVERLAPPED* overlapped)
{
    (void)titleId; (void)userIndex; (void)settingCount; (void)settings;
    return Complete(overlapped, X_ERROR_SUCCESS);
}

// ---- Enumeradores (sempre vazios: sem conquistas nem conteúdo baixável) ----

struct EmptyEnumerator final : KernelObject {};

static uint32_t XamUserCreateAchievementEnumerator(uint32_t titleId, uint32_t userIndex, uint64_t xuid, uint32_t flags,
                                                   uint32_t offset, uint32_t count, be<uint32_t>* bufferSize, be<uint32_t>* handle)
{
    (void)titleId; (void)userIndex; (void)xuid; (void)flags; (void)offset;
    if (bufferSize)
        bufferSize->set(count * 0x28);
    if (handle)
        handle->set(CreateHandle(std::make_shared<EmptyEnumerator>()));
    return X_ERROR_SUCCESS;
}

static uint32_t XamContentCreateEnumerator(uint32_t userIndex, uint32_t deviceId, uint32_t contentType, uint32_t contentFlags,
                                           uint32_t itemsPerEnumerate, be<uint32_t>* bufferSize, be<uint32_t>* handle)
{
    (void)userIndex; (void)deviceId; (void)contentType; (void)contentFlags;
    if (bufferSize)
        bufferSize->set(itemsPerEnumerate * uint32_t(sizeof(XCONTENT_DATA)));
    if (handle)
        handle->set(CreateHandle(std::make_shared<EmptyEnumerator>()));
    return X_ERROR_SUCCESS;
}

static uint32_t XamCreateEnumeratorHandle(uint32_t userIndex, uint32_t appId, uint32_t openMessage, uint32_t closeMessage,
                                          uint32_t extraSize, uint32_t itemCount, uint32_t flags, be<uint32_t>* handle)
{
    (void)userIndex; (void)appId; (void)openMessage; (void)closeMessage; (void)extraSize; (void)itemCount; (void)flags;
    if (handle)
        handle->set(CreateHandle(std::make_shared<EmptyEnumerator>()));
    return X_ERROR_SUCCESS;
}

static uint32_t XamEnumerate(uint32_t handle, uint32_t flags, void* buffer, uint32_t bufferSize, be<uint32_t>* itemsReturned, XXOVERLAPPED* overlapped)
{
    (void)handle; (void)flags; (void)buffer; (void)bufferSize;
    if (itemsReturned)
        itemsReturned->set(0);
    return Complete(overlapped, X_ERROR_NO_MORE_FILES);
}

static uint32_t XamGetPrivateEnumStructureFromHandle(uint32_t handle, be<uint32_t>* out)
{
    (void)handle;
    if (out)
        out->set(0);
    return X_STATUS_UNSUCCESSFUL;
}

// ---- Conteúdo (saves): cada pacote vira uma pasta em <dados>/content ----

// Disposições do XamContentCreate
enum : uint32_t { CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5 };

static fs::path ContentPath(const XCONTENT_DATA* content)
{
    std::string name(content->szFileName, strnlen(content->szFileName, sizeof(content->szFileName)));
    char type[16];
    snprintf(type, sizeof(type), "%08X", content->dwContentType.get());
    return DataDirectory() / "content" / type / (name.empty() ? "default" : name);
}

static uint32_t XamContentCreateEx(uint32_t userIndex, const char* rootName, XCONTENT_DATA* content, uint32_t flags,
                                   be<uint32_t>* disposition, be<uint32_t>* licenseMask, uint32_t cacheSize,
                                   uint64_t contentSize, XXOVERLAPPED* overlapped)
{
    (void)userIndex; (void)cacheSize; (void)contentSize;
    fs::path path = ContentPath(content);
    bool exists = fs::exists(path);
    uint32_t mode = flags & 0xF;

    uint32_t result = X_ERROR_SUCCESS;
    if ((mode == OPEN_EXISTING || mode == TRUNCATE_EXISTING) && !exists)
        result = X_ERROR_PATH_NOT_FOUND;
    else if (mode == CREATE_NEW && exists)
        result = X_ERROR_ALREADY_EXISTS;

    if (result == X_ERROR_SUCCESS)
    {
        if (exists && (mode == CREATE_ALWAYS || mode == TRUNCATE_EXISTING))
            fs::remove_all(path);
        MountDevice(std::string(rootName) + ":", path, true);
        if (disposition)
            disposition->set(exists && mode != CREATE_ALWAYS && mode != TRUNCATE_EXISTING ? 2 : 1); // 2 = aberto, 1 = criado
        if (licenseMask)
            licenseMask->set(0xFFFFFFFF);
    }
    fprintf(stderr, "[xam] XamContentCreateEx(\"%s\", \"%s\", modo %u) -> %u\n", rootName,
            content->szFileName, mode, result);
    return Complete(overlapped, result);
}

static uint32_t XamContentClose(const char* rootName, XXOVERLAPPED* overlapped)
{
    UnmountDevice(std::string(rootName) + ":");
    return Complete(overlapped, X_ERROR_SUCCESS);
}

static uint32_t XamContentDelete(uint32_t userIndex, XCONTENT_DATA* content, XXOVERLAPPED* overlapped)
{
    (void)userIndex;
    std::error_code ec;
    fs::remove_all(ContentPath(content), ec);
    return Complete(overlapped, X_ERROR_SUCCESS);
}

static uint32_t XamContentGetCreator(uint32_t userIndex, const XCONTENT_DATA* content, be<uint32_t>* isCreator,
                                     be<uint64_t>* xuid, XXOVERLAPPED* overlapped)
{
    (void)userIndex; (void)content;
    if (isCreator)
        isCreator->set(1);
    if (xuid)
        xuid->set(LOCAL_XUID);
    return Complete(overlapped, X_ERROR_SUCCESS);
}

static uint32_t XamContentGetDeviceName(uint32_t deviceId, be<uint16_t>* name, uint32_t nameCount)
{
    (void)deviceId;
    const char* label = "Hard Drive";
    uint32_t i = 0;
    for (; label[i] && i + 1 < nameCount; i++)
        name[i] = uint16_t(label[i]);
    if (nameCount)
        name[i] = 0;
    return X_ERROR_SUCCESS;
}

static uint32_t XamContentGetDeviceState(uint32_t deviceId, XXOVERLAPPED* overlapped)
{
    (void)deviceId;
    return Complete(overlapped, X_ERROR_SUCCESS);
}

static uint32_t XamContentGetLicenseMask(be<uint32_t>* mask, XXOVERLAPPED* overlapped)
{
    if (mask)
        mask->set(0xFFFFFFFF);
    return Complete(overlapped, X_ERROR_SUCCESS);
}

// ---- Interface do sistema (sem UI: respostas automáticas) ----

static uint32_t XamShowSigninUI(uint32_t count, uint32_t flags)
{
    (void)count; (void)flags;
    return X_ERROR_SUCCESS;
}

static uint32_t XamShowDeviceSelectorUI(uint32_t userIndex, uint32_t contentType, uint32_t contentFlags, uint64_t totalRequested,
                                        be<uint32_t>* deviceId, XXOVERLAPPED* overlapped)
{
    (void)userIndex; (void)contentType; (void)contentFlags; (void)totalRequested;
    if (deviceId)
        deviceId->set(1); // disco rígido
    return Complete(overlapped, X_ERROR_SUCCESS);
}

static uint32_t XamShowMessageBoxUI(uint32_t userIndex, uint32_t title, uint32_t text, uint32_t buttonCount,
                                    uint32_t buttons, uint32_t focus, uint32_t flags, be<uint32_t>* result, XXOVERLAPPED* overlapped)
{
    (void)userIndex; (void)title; (void)text; (void)buttonCount; (void)buttons; (void)flags;
    fprintf(stderr, "[xam] XamShowMessageBoxUI: respondendo com o botão %u\n", focus);
    if (result)
        result->set(focus);
    return Complete(overlapped, X_ERROR_SUCCESS);
}

static uint32_t XamShowMessageBoxUIEx(uint32_t userIndex, uint32_t title, uint32_t text, uint32_t buttonCount,
                                      uint32_t buttons, uint32_t focus, uint32_t flags, uint32_t unknown,
                                      be<uint32_t>* result, XXOVERLAPPED* overlapped)
{
    (void)unknown;
    return XamShowMessageBoxUI(userIndex, title, text, buttonCount, buttons, focus, flags, result, overlapped);
}

// ---- Notificações ----

struct NotificationListener final : KernelObject
{
    uint64_t mask = 0;
};

static uint32_t XamNotifyCreateListener(uint64_t mask, uint32_t maxVersion)
{
    (void)maxVersion;
    auto listener = std::make_shared<NotificationListener>();
    listener->mask = mask;
    return CreateHandle(listener);
}

// ---- Input (#15): controle físico ou teclado via SDL (host/platform.cpp) ----

static bool Connected(uint32_t userIndex)
{
    return IsPadConnected(userIndex);
}

static void FillCapabilities(XAMINPUT_CAPABILITIES* caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->Type = XAMINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XAMINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Flags = 0;
    caps->Gamepad.wButtons = 0xFFFF;
    caps->Gamepad.bLeftTrigger = 0xFF;
    caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = int16_t(0xFFC0);
    caps->Gamepad.sThumbLY = int16_t(0xFFC0);
    caps->Gamepad.sThumbRX = int16_t(0xFFC0);
    caps->Gamepad.sThumbRY = int16_t(0xFFC0);
    caps->Vibration.wLeftMotorSpeed = 0xFFFF;
    caps->Vibration.wRightMotorSpeed = 0xFFFF;
    // Estruturas do guest em big-endian.
    caps->Flags = __builtin_bswap16(caps->Flags);
    caps->Gamepad.wButtons = __builtin_bswap16(caps->Gamepad.wButtons);
    caps->Gamepad.sThumbLX = int16_t(__builtin_bswap16(uint16_t(caps->Gamepad.sThumbLX)));
    caps->Gamepad.sThumbLY = int16_t(__builtin_bswap16(uint16_t(caps->Gamepad.sThumbLY)));
    caps->Gamepad.sThumbRX = int16_t(__builtin_bswap16(uint16_t(caps->Gamepad.sThumbRX)));
    caps->Gamepad.sThumbRY = int16_t(__builtin_bswap16(uint16_t(caps->Gamepad.sThumbRY)));
}

static uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* caps)
{
    (void)flags;
    if (!Connected(userIndex))
        return X_ERROR_DEVICE_NOT_CONNECTED;
    FillCapabilities(caps);
    return X_ERROR_SUCCESS;
}

static uint32_t XamInputGetCapabilitiesEx(uint32_t unknown, uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* caps)
{
    (void)unknown;
    return XamInputGetCapabilities(userIndex, flags, caps);
}

static uint32_t XamInputGetState(uint32_t userIndex, uint32_t flags, XAMINPUT_STATE* state)
{
    (void)flags;
    if (!Connected(userIndex))
        return X_ERROR_DEVICE_NOT_CONNECTED;
    static std::atomic<uint32_t> packet{ 0 };
    static PadState last;
    PadState pad = GetPadState();
    if (memcmp(&pad, &last, sizeof(pad)) != 0)
    {
        last = pad;
        packet++; // o número do pacote muda quando o estado muda
    }
    // Estrutura do guest em big-endian.
    state->dwPacketNumber = __builtin_bswap32(packet.load());
    state->Gamepad.wButtons = __builtin_bswap16(pad.buttons);
    state->Gamepad.bLeftTrigger = pad.leftTrigger;
    state->Gamepad.bRightTrigger = pad.rightTrigger;
    state->Gamepad.sThumbLX = int16_t(__builtin_bswap16(uint16_t(pad.thumbLX)));
    state->Gamepad.sThumbLY = int16_t(__builtin_bswap16(uint16_t(pad.thumbLY)));
    state->Gamepad.sThumbRX = int16_t(__builtin_bswap16(uint16_t(pad.thumbRX)));
    state->Gamepad.sThumbRY = int16_t(__builtin_bswap16(uint16_t(pad.thumbRY)));
    return X_ERROR_SUCCESS;
}

static uint32_t XamInputSetState(uint32_t userIndex, uint32_t flags, XAMINPUT_VIBRATION* vibration)
{
    (void)flags;
    if (!Connected(userIndex))
        return X_ERROR_DEVICE_NOT_CONNECTED;
    if (vibration)
        SetPadVibration(__builtin_bswap16(vibration->wLeftMotorSpeed), __builtin_bswap16(vibration->wRightMotorSpeed));
    return X_ERROR_SUCCESS;
}

static uint32_t XamInputRawState(uint32_t userIndex, uint32_t flags, void* state)
{
    (void)flags; (void)state;
    return Connected(userIndex) ? X_ERROR_FUNCTION_FAILED : X_ERROR_DEVICE_NOT_CONNECTED;
}

GUEST_FUNCTION_HOOK(__imp__XamGetSystemVersion, XamGetSystemVersion);
GUEST_FUNCTION_HOOK(__imp__XamGetExecutionId, XamGetExecutionId);
GUEST_FUNCTION_HOOK(__imp__XamLoaderTerminateTitle, XamLoaderTerminateTitle);
GUEST_FUNCTION_HOOK(__imp__XamLoaderLaunchTitle, XamLoaderLaunchTitle);
GUEST_FUNCTION_HOOK(__imp__XamUserGetSigninState, XamUserGetSigninState);
GUEST_FUNCTION_HOOK(__imp__XamUserGetSigninInfo, XamUserGetSigninInfo);
GUEST_FUNCTION_HOOK(__imp__XamUserGetXUID, XamUserGetXUID);
GUEST_FUNCTION_HOOK(__imp__XamUserReadProfileSettings, XamUserReadProfileSettings);
GUEST_FUNCTION_HOOK(__imp__XamUserWriteProfileSettings, XamUserWriteProfileSettings);
GUEST_FUNCTION_HOOK(__imp__XamUserCreateAchievementEnumerator, XamUserCreateAchievementEnumerator);
GUEST_FUNCTION_HOOK(__imp__XamContentCreateEnumerator, XamContentCreateEnumerator);
GUEST_FUNCTION_HOOK(__imp__XamCreateEnumeratorHandle, XamCreateEnumeratorHandle);
GUEST_FUNCTION_HOOK(__imp__XamEnumerate, XamEnumerate);
GUEST_FUNCTION_HOOK(__imp__XamGetPrivateEnumStructureFromHandle, XamGetPrivateEnumStructureFromHandle);
GUEST_FUNCTION_HOOK(__imp__XamContentCreateEx, XamContentCreateEx);
GUEST_FUNCTION_HOOK(__imp__XamContentClose, XamContentClose);
GUEST_FUNCTION_HOOK(__imp__XamContentDelete, XamContentDelete);
GUEST_FUNCTION_HOOK(__imp__XamContentGetCreator, XamContentGetCreator);
GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceName, XamContentGetDeviceName);
GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceState, XamContentGetDeviceState);
GUEST_FUNCTION_HOOK(__imp__XamContentGetLicenseMask, XamContentGetLicenseMask);
GUEST_FUNCTION_HOOK(__imp__XamShowSigninUI, XamShowSigninUI);
GUEST_FUNCTION_HOOK(__imp__XamShowDeviceSelectorUI, XamShowDeviceSelectorUI);
GUEST_FUNCTION_HOOK(__imp__XamShowMessageBoxUI, XamShowMessageBoxUI);
GUEST_FUNCTION_HOOK(__imp__XamShowMessageBoxUIEx, XamShowMessageBoxUIEx);
GUEST_FUNCTION_HOOK(__imp__XamNotifyCreateListener, XamNotifyCreateListener);
GUEST_FUNCTION_HOOK(__imp__XamInputGetCapabilities, XamInputGetCapabilities);
GUEST_FUNCTION_HOOK(__imp__XamInputGetCapabilitiesEx, XamInputGetCapabilitiesEx);
GUEST_FUNCTION_HOOK(__imp__XamInputGetState, XamInputGetState);
GUEST_FUNCTION_HOOK(__imp__XamInputSetState, XamInputSetState);
GUEST_FUNCTION_HOOK(__imp__XamInputRawState, XamInputRawState);
