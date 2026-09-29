// Sistema de arquivos do guest: NtCreateFile/NtReadFile/... sobre a pasta do
// dump do jogo. Semântica e layouts de estrutura seguem o Xenia
// (xboxkrnl_io.cc, xboxkrnl_io_info.cc, info/file.h, info/volume.h, xfile.h).
//
// Mapeamento:
//   game:\  d:\  \Device\Cdrom0\       -> pasta do jogo (RAYMAN_GAME_DIR, padrão private/game)
//   cache:\  \Device\Harddisk0\Cache0\ -> pasta gravável (RAYMAN_DATA_DIR/cache)
//   links criados com ObCreateSymbolicLink
// A busca não diferencia maiúsculas de minúsculas (o Android diferencia).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include "file_system.h"
#include "function.h"
#include "objects.h"
#include "xbox_defs.h"

namespace fs = std::filesystem;

constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t STATUS_OBJECT_NAME_INVALID = 0xC0000033;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
constexpr uint32_t STATUS_INVALID_INFO_CLASS = 0xC0000003;
constexpr uint32_t STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA;
constexpr uint32_t STATUS_NOT_A_DIRECTORY = 0xC0000103;

// Disposições e opções de NtCreateFile
enum : uint32_t { FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2, FILE_OPEN_IF = 3, FILE_OVERWRITE = 4, FILE_OVERWRITE_IF = 5 };
enum : uint32_t { FILE_SUPERSEDED = 0, FILE_OPENED = 1, FILE_CREATED = 2, FILE_OVERWRITTEN = 3, FILE_DOES_NOT_EXIST = 5 };
constexpr uint32_t FILE_DIRECTORY_FILE = 0x00000001;
constexpr uint32_t FILE_NON_DIRECTORY_FILE = 0x00000040;
constexpr uint32_t FILE_ATTRIBUTE_READONLY = 0x01;
constexpr uint32_t FILE_ATTRIBUTE_DIRECTORY = 0x10;
constexpr uint32_t FILE_ATTRIBUTE_NORMAL = 0x80;

// Classes de informação (X_FILE_INFORMATION_CLASS)
enum : uint32_t
{
    XFileDirectoryInformation = 1, XFileBasicInformation = 4, XFileStandardInformation = 5,
    XFileInternalInformation = 6, XFileRenameInformation = 10, XFileDispositionInformation = 13,
    XFilePositionInformation = 14, XFileModeInformation = 16, XFileAlignmentInformation = 17,
    XFileAllocationInformation = 19, XFileEndOfFileInformation = 20, XFileSectorInformation = 26,
    XFileIoPriorityInformation = 32, XFileNetworkOpenInformation = 34,
};
enum : uint32_t { XFileFsVolumeInformation = 1, XFileFsSizeInformation = 3, XFileFsDeviceInformation = 4, XFileFsAttributeInformation = 5 };

#pragma pack(push, 1)
struct X_FILE_NETWORK_OPEN_INFORMATION
{
    be<uint64_t> creationTime, lastAccessTime, lastWriteTime, changeTime;
    be<uint64_t> allocationSize, endOfFile;
    be<uint32_t> attributes, pad;
};
struct X_FILE_STANDARD_INFORMATION
{
    be<uint64_t> allocationSize, endOfFile;
    be<uint32_t> numberOfLinks;
    uint8_t deletePending, directory;
    uint8_t pad[2];
};
struct X_FILE_BASIC_INFORMATION
{
    be<uint64_t> creationTime, lastAccessTime, lastWriteTime, changeTime;
    be<uint32_t> attributes, pad;
};
struct X_FILE_DIRECTORY_INFORMATION
{
    be<uint32_t> nextEntryOffset, fileIndex;
    be<uint64_t> creationTime, lastAccessTime, lastWriteTime, changeTime;
    be<uint64_t> endOfFile, allocationSize;
    be<uint32_t> attributes, fileNameLength;
    char fileName[1];
};
struct X_FILE_FS_SIZE_INFORMATION
{
    be<uint64_t> totalAllocationUnits, availableAllocationUnits;
    be<uint32_t> sectorsPerAllocationUnit, bytesPerSector;
};
struct X_FILE_FS_ATTRIBUTE_INFORMATION
{
    be<uint32_t> attributes;
    be<int32_t> componentNameMaxLength;
    be<uint32_t> nameLength;
    char name[1];
};
struct X_FILE_FS_DEVICE_INFORMATION
{
    be<uint32_t> deviceType, characteristics;
};
#pragma pack(pop)
static_assert(sizeof(X_FILE_NETWORK_OPEN_INFORMATION) == 56);
static_assert(sizeof(X_FILE_DIRECTORY_INFORMATION) == 0x41);

// ---- Dispositivos e caminhos ----

struct Device
{
    fs::path root;
    bool writable;
};

static std::mutex g_fsMutex;
static std::unordered_map<std::string, Device> g_devices;          // "game:" -> pasta
static std::unordered_map<std::string, std::string> g_symlinks;    // "cache:" -> "\device\harddisk0\cache0"

#if defined(__SWITCH__)
constexpr const char* DEFAULT_GAME_DIR = "sdmc:/switch/RaymanOriginsRecomp/game";
constexpr const char* DEFAULT_DATA_DIR = "sdmc:/switch/RaymanOriginsRecomp/data";
#else
constexpr const char* DEFAULT_GAME_DIR = "private/game";
constexpr const char* DEFAULT_DATA_DIR = "private/data";
#endif

static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
    return s;
}

static void InitDevices()
{
    static std::once_flag once;
    std::call_once(once, [] {
        const char* game = getenv("RAYMAN_GAME_DIR");
        const char* data = getenv("RAYMAN_DATA_DIR");
        fs::path gameDir = game ? game : DEFAULT_GAME_DIR;
        fs::path dataDir = data ? data : DEFAULT_DATA_DIR;
        fs::create_directories(dataDir / "cache");

        for (const char* name : { "game:", "d:", "\\device\\cdrom0" })
            g_devices[name] = { gameDir, false };
        for (const char* name : { "cache:", "\\device\\harddisk0\\cache0" })
            g_devices[name] = { dataDir / "cache", true };

        fprintf(stderr, "[fs] game:\\ -> %s\n", fs::absolute(gameDir).c_str());
    });
}

fs::path DataDirectory()
{
    const char* data = getenv("RAYMAN_DATA_DIR");
    return data ? data : DEFAULT_DATA_DIR;
}

void MountDevice(const std::string& name, const fs::path& root, bool writable)
{
    InitDevices();
    std::error_code ec;
    fs::create_directories(root, ec);
    std::lock_guard lock(g_fsMutex);
    g_devices[Lower(name)] = { root, writable };
    fprintf(stderr, "[fs] montado %s -> %s\n", name.c_str(), root.c_str());
}

void UnmountDevice(const std::string& name)
{
    std::lock_guard lock(g_fsMutex);
    g_devices.erase(Lower(name));
}

// Procura "name" em "dir" sem diferenciar maiúsculas; devolve o caminho real ou vazio.
static fs::path FindCaseInsensitive(const fs::path& dir, const std::string& name)
{
    fs::path direct = dir / name;
    if (fs::exists(direct))
        return direct;
    std::string wanted = Lower(name);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec))
        if (Lower(entry.path().filename().string()) == wanted)
            return entry.path();
    return {};
}

struct ResolvedPath
{
    fs::path host;          // caminho no host (pode não existir)
    bool parentExists = false;
    bool writable = false;
    bool valid = false;
};

// Converte um caminho do guest ("game:\\foo\\bar.ipk") para o host.
static ResolvedPath ResolveGuestPath(std::string path, const fs::path* relativeTo = nullptr, bool relativeWritable = false)
{
    InitDevices();
    std::replace(path.begin(), path.end(), '/', '\\');
    if (path.rfind("\\??\\", 0) == 0)
        path = path.substr(4);

    ResolvedPath result;
    fs::path root;
    std::string rest;

    if (relativeTo)
    {
        root = *relativeTo;
        rest = path;
        result.writable = relativeWritable;
    }
    else
    {
        std::lock_guard lock(g_fsMutex);
        std::string lower = Lower(path);

        // Links simbólicos (ObCreateSymbolicLink) primeiro.
        for (const auto& [link, target] : g_symlinks)
        {
            if (lower.rfind(link, 0) == 0)
            {
                lower = target + lower.substr(link.size());
                path = target + path.substr(link.size());
                break;
            }
        }

        // Dispositivo mais longo que casa com o prefixo.
        size_t bestLength = 0;
        for (const auto& [name, device] : g_devices)
        {
            if (lower.rfind(name, 0) == 0 && name.size() > bestLength)
            {
                bestLength = name.size();
                root = device.root;
                result.writable = device.writable;
            }
        }
        if (bestLength == 0)
            return result;
        rest = path.substr(bestLength);
    }

    // Anda componente a componente, corrigindo maiúsculas/minúsculas.
    fs::path current = root;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= rest.size())
    {
        size_t end = rest.find('\\', start);
        if (end == std::string::npos)
            end = rest.size();
        std::string part = rest.substr(start, end - start);
        if (!part.empty() && part != ".")
        {
            if (part == "..")
            {
                if (!parts.empty())
                    parts.pop_back();
            }
            else
                parts.push_back(part);
        }
        start = end + 1;
    }

    for (size_t i = 0; i < parts.size(); i++)
    {
        fs::path found = FindCaseInsensitive(current, parts[i]);
        if (found.empty())
        {
            // Só o último componente pode não existir (criação de arquivo).
            result.parentExists = (i + 1 == parts.size()) && fs::is_directory(current);
            result.host = current / parts[i];
            result.valid = true;
            return result;
        }
        current = found;
    }
    result.host = current;
    result.parentExists = true;
    result.valid = true;
    return result;
}

static std::string AnsiToString(const XANSI_STRING* ansi)
{
    if (!ansi || ansi->Buffer.ptr.get() == 0)
        return {};
    return std::string(ansi->Buffer.get(), ansi->Length.get());
}

// ---- Objeto de arquivo ----

struct XFile final : KernelObject
{
    std::string guestPath;
    fs::path host;
    int fd = -1;
    bool directory = false;
    bool writable = false;
    uint64_t position = 0;

    std::vector<fs::directory_entry> listing; // NtQueryDirectoryFile
    size_t listingIndex = 0;
    bool listingLoaded = false;

    ~XFile() override
    {
        if (fd >= 0)
            close(fd);
    }
    bool IsSignaled() const override { return true; } // I/O sempre síncrono
};

static uint64_t ToFileTime(const struct timespec& ts)
{
    constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;
    return uint64_t(int64_t(ts.tv_sec) * 10000000 + ts.tv_nsec / 100 + FILETIME_EPOCH_DIFFERENCE);
}

struct FileAttributes
{
    uint64_t creation, access, write, size;
    uint32_t attributes;
};

static bool StatPath(const fs::path& path, bool writable, FileAttributes& out)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
#ifdef __APPLE__
    out.creation = ToFileTime(st.st_birthtimespec);
    out.access = ToFileTime(st.st_atimespec);
    out.write = ToFileTime(st.st_mtimespec);
#else
    out.creation = ToFileTime(st.st_ctim);
    out.access = ToFileTime(st.st_atim);
    out.write = ToFileTime(st.st_mtim);
#endif
    bool isDir = S_ISDIR(st.st_mode);
    out.size = isDir ? 0 : uint64_t(st.st_size);
    out.attributes = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!writable)
        out.attributes |= FILE_ATTRIBUTE_READONLY;
    return true;
}

static void FillNetworkOpenInfo(X_FILE_NETWORK_OPEN_INFORMATION* info, const FileAttributes& a)
{
    info->creationTime = a.creation;
    info->lastAccessTime = a.access;
    info->lastWriteTime = a.write;
    info->changeTime = a.write;
    info->allocationSize = (a.size + 0xFFF) & ~uint64_t(0xFFF);
    info->endOfFile = a.size;
    info->attributes = a.attributes;
    info->pad = 0;
}

static void SetIoStatus(XIO_STATUS_BLOCK* iosb, uint32_t status, uint32_t information)
{
    if (iosb)
    {
        iosb->Status = status;
        iosb->Information = information;
    }
}

// ---- Imports ----

static uint32_t NtCreateFile(be<uint32_t>* handle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes,
                             XIO_STATUS_BLOCK* iosb, be<uint64_t>* allocationSize, uint32_t fileAttributes,
                             uint32_t shareAccess, uint32_t disposition, uint32_t createOptions)
{
    if (handle)
        handle->set(INVALID_HANDLE_VALUE);
    if (!attributes)
        return X_STATUS_INVALID_PARAMETER;

    std::string guestPath = AnsiToString(attributes->Name.get());

    // Caminho relativo a um diretório já aberto.
    ResolvedPath resolved;
    uint32_t rootHandle = attributes->RootDirectory.get();
    if (rootHandle != 0 && rootHandle != 0xFFFFFFFD)
    {
        auto root = GetObjectAs<XFile>(rootHandle);
        if (!root)
            return STATUS_INVALID_HANDLE;
        resolved = ResolveGuestPath(guestPath, &root->host, root->writable);
        guestPath = root->guestPath + "\\" + guestPath;
    }
    else
    {
        resolved = ResolveGuestPath(guestPath);
    }

    uint32_t status = X_STATUS_SUCCESS;
    uint32_t action = FILE_OPENED;
    bool wantDir = createOptions & FILE_DIRECTORY_FILE;
    bool wantFile = createOptions & FILE_NON_DIRECTORY_FILE;
    std::error_code ec;
    bool exists = resolved.valid && fs::exists(resolved.host, ec);
    bool isDir = exists && fs::is_directory(resolved.host, ec);

    if (!resolved.valid)
        status = STATUS_OBJECT_PATH_NOT_FOUND;
    else if (!exists && !resolved.parentExists)
        status = STATUS_OBJECT_PATH_NOT_FOUND;
    else if (exists && disposition == FILE_CREATE)
        status = STATUS_OBJECT_NAME_COLLISION;
    else if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE))
        status = STATUS_OBJECT_NAME_NOT_FOUND;
    else if (exists && wantDir && !isDir)
        status = STATUS_NOT_A_DIRECTORY;
    else if (exists && wantFile && isDir)
        status = STATUS_FILE_IS_A_DIRECTORY;
    else if (!exists && !resolved.writable)
        status = X_STATUS_ACCESS_DENIED;

    std::shared_ptr<XFile> file;
    if (status == X_STATUS_SUCCESS)
    {
        file = std::make_shared<XFile>();
        file->guestPath = guestPath;
        file->host = resolved.host;
        file->writable = resolved.writable;

        if (!exists && wantDir)
        {
            fs::create_directory(resolved.host, ec);
            file->directory = true;
            action = FILE_CREATED;
        }
        else if (isDir)
        {
            file->directory = true;
        }
        else
        {
            int flags = resolved.writable ? O_RDWR : O_RDONLY;
            if (!exists)
            {
                flags |= O_CREAT;
                action = FILE_CREATED;
            }
            else if (resolved.writable && (disposition == FILE_SUPERSEDE || disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF))
            {
                flags |= O_TRUNC;
                action = disposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
            }
            file->fd = open(resolved.host.c_str(), flags, 0644);
            if (file->fd < 0)
                status = X_STATUS_ACCESS_DENIED;
        }
    }

    fprintf(stderr, "[fs] open %-52s -> %s\n", guestPath.c_str(),
            status == X_STATUS_SUCCESS ? (file->directory ? "OK (dir)" : "OK") :
            status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND ? "não encontrado" : "erro");

    if (status == X_STATUS_SUCCESS)
    {
        uint32_t h = CreateHandle(file);
        if (handle)
            handle->set(h);
        SetIoStatus(iosb, status, action);
    }
    else
    {
        SetIoStatus(iosb, status, FILE_DOES_NOT_EXIST);
    }
    (void)desiredAccess; (void)allocationSize; (void)fileAttributes; (void)shareAccess;
    return status;
}

static uint32_t NtOpenFile(be<uint32_t>* handle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes,
                           XIO_STATUS_BLOCK* iosb, uint32_t openOptions)
{
    return NtCreateFile(handle, desiredAccess, attributes, iosb, nullptr, 0, 0, FILE_OPEN, openOptions);
}

static void SignalEvent(uint32_t eventHandle)
{
    if (eventHandle == 0)
        return;
    if (auto event = GetObjectAs<Event>(eventHandle))
    {
        {
            auto lock = LockDispatcher();
            event->signaled = true;
            SyncHeaderSignalState(*event);
        }
        NotifyDispatcher();
    }
}

// byteOffset nulo ou FILE_USE_FILE_POINTER_POSITION (-2) = posição atual.
static uint64_t EffectiveOffset(const XFile& file, const be<uint64_t>* byteOffset)
{
    if (!byteOffset)
        return file.position;
    uint64_t value = byteOffset->get();
    return value == 0xFFFFFFFFFFFFFFFEull ? file.position : value;
}

// newlib on Switch exposes seek/read/write but not pread/pwrite.  Serialise
// the seek-and-I/O pair so the guest's offset-based calls retain POSIX pread /
// pwrite semantics when guest threads access different files concurrently.
static ssize_t ReadAt(const XFile& file, void* buffer, size_t length, uint64_t offset)
{
#if defined(__SWITCH__)
    std::lock_guard lock(g_fsMutex);
    if (lseek(file.fd, static_cast<off_t>(offset), SEEK_SET) < 0)
        return -1;
    return read(file.fd, buffer, length);
#else
    return pread(file.fd, buffer, length, static_cast<off_t>(offset));
#endif
}

static ssize_t WriteAt(const XFile& file, const void* buffer, size_t length, uint64_t offset)
{
#if defined(__SWITCH__)
    std::lock_guard lock(g_fsMutex);
    if (lseek(file.fd, static_cast<off_t>(offset), SEEK_SET) < 0)
        return -1;
    return write(file.fd, buffer, length);
#else
    return pwrite(file.fd, buffer, length, static_cast<off_t>(offset));
#endif
}

static uint32_t NtReadFile(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
                           XIO_STATUS_BLOCK* iosb, void* buffer, uint32_t length, be<uint64_t>* byteOffset)
{
    auto file = GetObjectAs<XFile>(fileHandle);
    if (!file || file->fd < 0)
    {
        SetIoStatus(iosb, STATUS_INVALID_HANDLE, 0);
        return STATUS_INVALID_HANDLE;
    }

    uint64_t offset = EffectiveOffset(*file, byteOffset);
    ssize_t read = ReadAt(*file, buffer, length, offset);
    uint32_t status = X_STATUS_SUCCESS;
    if (read < 0)
    {
        status = X_STATUS_UNSUCCESSFUL;
        read = 0;
    }
    else if (read == 0 && length > 0)
    {
        status = STATUS_END_OF_FILE;
    }
    file->position = offset + uint64_t(read);

    SetIoStatus(iosb, status, uint32_t(read));
    SignalEvent(eventHandle);
    if (apcRoutine & ~1u)
        fprintf(stderr, "[fs] NtReadFile com APC 0x%08X (ctx 0x%08X) ainda não entregue\n", apcRoutine, apcContext);
    return status;
}

static uint32_t NtReadFileScatter(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
                                  XIO_STATUS_BLOCK* iosb, be<uint32_t>* segments, uint32_t length, be<uint64_t>* byteOffset)
{
    auto file = GetObjectAs<XFile>(fileHandle);
    if (!file || file->fd < 0)
    {
        SetIoStatus(iosb, STATUS_INVALID_HANDLE, 0);
        return STATUS_INVALID_HANDLE;
    }

    // Cada segmento é uma página de 4 KB do guest.
    uint64_t offset = EffectiveOffset(*file, byteOffset);
    uint32_t total = 0;
    for (uint32_t i = 0; total < length; i++)
    {
        uint32_t chunk = std::min<uint32_t>(0x1000, length - total);
        ssize_t read = ReadAt(*file, g_memory.Translate(segments[i].get()), chunk, offset + total);
        if (read <= 0)
            break;
        total += uint32_t(read);
        if (uint32_t(read) < chunk)
            break;
    }
    file->position = offset + total;

    uint32_t status = (total == 0 && length > 0) ? STATUS_END_OF_FILE : X_STATUS_SUCCESS;
    SetIoStatus(iosb, status, total);
    SignalEvent(eventHandle);
    (void)apcRoutine; (void)apcContext;
    return status;
}

static uint32_t NtWriteFile(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
                            XIO_STATUS_BLOCK* iosb, const void* buffer, uint32_t length, be<uint64_t>* byteOffset)
{
    auto file = GetObjectAs<XFile>(fileHandle);
    if (!file || file->fd < 0 || !file->writable)
    {
        SetIoStatus(iosb, X_STATUS_ACCESS_DENIED, 0);
        return file ? X_STATUS_ACCESS_DENIED : STATUS_INVALID_HANDLE;
    }

    uint64_t offset = EffectiveOffset(*file, byteOffset);
    ssize_t written = WriteAt(*file, buffer, length, offset);
    uint32_t status = written < 0 ? X_STATUS_UNSUCCESSFUL : X_STATUS_SUCCESS;
    if (written < 0)
        written = 0;
    file->position = offset + uint64_t(written);

    SetIoStatus(iosb, status, uint32_t(written));
    SignalEvent(eventHandle);
    (void)apcRoutine; (void)apcContext;
    return status;
}

static uint32_t NtQueryInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* iosb, void* info, uint32_t length, uint32_t infoClass)
{
    auto file = GetObjectAs<XFile>(fileHandle);
    if (!file)
        return STATUS_INVALID_HANDLE;

    FileAttributes a{};
    StatPath(file->host, file->writable, a);
    memset(info, 0, length);

    uint32_t status = X_STATUS_SUCCESS, out = 0;
    switch (infoClass)
    {
    case XFileStandardInformation:
    {
        if (length < sizeof(X_FILE_STANDARD_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
        auto* s = static_cast<X_FILE_STANDARD_INFORMATION*>(info);
        s->allocationSize = (a.size + 0xFFF) & ~uint64_t(0xFFF);
        s->endOfFile = a.size;
        s->numberOfLinks = 1;
        s->directory = file->directory ? 1 : 0;
        out = sizeof(*s);
        break;
    }
    case XFileInternalInformation:
        if (length < 8) return STATUS_INFO_LENGTH_MISMATCH;
        *static_cast<be<uint64_t>*>(info) = uint64_t(std::hash<std::string>{}(file->host.string()));
        out = 8;
        break;
    case XFilePositionInformation:
        if (length < 8) return STATUS_INFO_LENGTH_MISMATCH;
        *static_cast<be<uint64_t>*>(info) = file->position;
        out = 8;
        break;
    case XFileAlignmentInformation:
        if (length < 4) return STATUS_INFO_LENGTH_MISMATCH;
        out = 4;
        break;
    case XFileSectorInformation:
    {
        if (length < 4) return STATUS_INFO_LENGTH_MISMATCH;
        size_t hash = std::hash<std::string>{}(file->host.string());
        *static_cast<be<uint32_t>*>(info) = uint32_t(hash ^ (hash >> 32));
        out = 4;
        break;
    }
    case XFileNetworkOpenInformation:
        if (length < sizeof(X_FILE_NETWORK_OPEN_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
        FillNetworkOpenInfo(static_cast<X_FILE_NETWORK_OPEN_INFORMATION*>(info), a);
        out = sizeof(X_FILE_NETWORK_OPEN_INFORMATION);
        break;
    default:
        fprintf(stderr, "[fs] NtQueryInformationFile classe %u não suportada\n", infoClass);
        status = STATUS_INVALID_INFO_CLASS;
        break;
    }
    SetIoStatus(iosb, status, out);
    return status;
}

static uint32_t NtSetInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* iosb, void* info, uint32_t length, uint32_t infoClass)
{
    auto file = GetObjectAs<XFile>(fileHandle);
    if (!file)
        return STATUS_INVALID_HANDLE;

    uint32_t status = X_STATUS_SUCCESS, out = 0;
    switch (infoClass)
    {
    case XFilePositionInformation:
        if (length < 8) return STATUS_INFO_LENGTH_MISMATCH;
        file->position = static_cast<be<uint64_t>*>(info)->get();
        out = 8;
        break;
    case XFileEndOfFileInformation:
        if (length < 8) return STATUS_INFO_LENGTH_MISMATCH;
        if (!file->writable || file->fd < 0 || ftruncate(file->fd, off_t(static_cast<be<uint64_t>*>(info)->get())) != 0)
            status = X_STATUS_ACCESS_DENIED;
        out = 8;
        break;
    case XFileBasicInformation:
    case XFileDispositionInformation:
    case XFileAllocationInformation:
    case XFileModeInformation:
    case XFileIoPriorityInformation:
        out = length; // aceitos sem efeito
        break;
    default:
        fprintf(stderr, "[fs] NtSetInformationFile classe %u não suportada\n", infoClass);
        status = STATUS_INVALID_INFO_CLASS;
        break;
    }
    SetIoStatus(iosb, status, out);
    return status;
}

static uint32_t NtQueryFullAttributesFile(XOBJECT_ATTRIBUTES* attributes, X_FILE_NETWORK_OPEN_INFORMATION* info)
{
    if (!attributes)
        return X_STATUS_INVALID_PARAMETER;
    std::string guestPath = AnsiToString(attributes->Name.get());
    ResolvedPath resolved = ResolveGuestPath(guestPath);
    FileAttributes a{};
    bool found = resolved.valid && StatPath(resolved.host, resolved.writable, a);
    fprintf(stderr, "[fs] stat %-52s -> %s\n", guestPath.c_str(), found ? "OK" : "não encontrado");
    if (!found)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    FillNetworkOpenInfo(info, a);
    return X_STATUS_SUCCESS;
}

static uint32_t NtQueryVolumeInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* iosb, void* info, uint32_t length, uint32_t infoClass)
{
    auto file = GetObjectAs<XFile>(fileHandle);
    if (!file)
        return STATUS_INVALID_HANDLE;
    memset(info, 0, length);

    uint32_t status = X_STATUS_SUCCESS, out = 0;
    switch (infoClass)
    {
    case XFileFsVolumeInformation:
        if (length < 17) return STATUS_INFO_LENGTH_MISMATCH;
        out = 17; // até o campo label, sem rótulo
        break;
    case XFileFsSizeInformation:
    {
        if (length < sizeof(X_FILE_FS_SIZE_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
        auto* s = static_cast<X_FILE_FS_SIZE_INFORMATION*>(info);
        s->totalAllocationUnits = 0x100000;     // 4 GB em unidades de 4 KB
        s->availableAllocationUnits = 0x80000;
        s->sectorsPerAllocationUnit = 8;
        s->bytesPerSector = 0x200;
        out = sizeof(*s);
        break;
    }
    case XFileFsDeviceInformation:
    {
        if (length < sizeof(X_FILE_FS_DEVICE_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
        static_cast<X_FILE_FS_DEVICE_INFORMATION*>(info)->deviceType = 0x22; // FILE_DEVICE_UNKNOWN
        out = sizeof(X_FILE_FS_DEVICE_INFORMATION);
        break;
    }
    case XFileFsAttributeInformation:
    {
        if (length < 12) return STATUS_INFO_LENGTH_MISMATCH;
        auto* s = static_cast<X_FILE_FS_ATTRIBUTE_INFORMATION*>(info);
        const char* name = file->writable ? "FATX" : "GDFX";
        s->attributes = 0;
        s->componentNameMaxLength = 255;
        s->nameLength = 4;
        if (length >= 12 + 4)
        {
            memcpy(s->name, name, 4);
            out = 16;
        }
        else
        {
            status = 0x80000005; // BUFFER_OVERFLOW
            out = 12;
        }
        break;
    }
    default:
        fprintf(stderr, "[fs] NtQueryVolumeInformationFile classe %u não suportada\n", infoClass);
        status = STATUS_INVALID_INFO_CLASS;
        break;
    }
    SetIoStatus(iosb, status, out);
    return status;
}

// Casa um nome com um padrão de "*" e "?" sem diferenciar maiúsculas.
static bool MatchPattern(const std::string& name, const std::string& pattern)
{
    if (pattern.empty() || pattern == "*" || pattern == "*.*")
        return true;
    std::string n = Lower(name), p = Lower(pattern);
    size_t ni = 0, pi = 0, star = std::string::npos, mark = 0;
    while (ni < n.size())
    {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == n[ni])) { ni++; pi++; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ni; }
        else if (star != std::string::npos) { pi = star + 1; ni = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*')
        pi++;
    return pi == p.size();
}

static uint32_t NtQueryDirectoryFile(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
                                     XIO_STATUS_BLOCK* iosb, X_FILE_DIRECTORY_INFORMATION* info, uint32_t length,
                                     XANSI_STRING* fileName, uint32_t restartScan)
{
    if (length < 72)
        return STATUS_INFO_LENGTH_MISMATCH;
    auto file = GetObjectAs<XFile>(fileHandle);
    if (!file || !file->directory)
    {
        SetIoStatus(iosb, STATUS_NO_SUCH_FILE, 0);
        return STATUS_NO_SUCH_FILE;
    }

    std::string pattern = AnsiToString(fileName);
    if (restartScan || !file->listingLoaded)
    {
        file->listing.clear();
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(file->host, ec))
            if (MatchPattern(entry.path().filename().string(), pattern))
                file->listing.push_back(entry);
        std::sort(file->listing.begin(), file->listing.end());
        file->listingIndex = 0;
        file->listingLoaded = true;
    }

    uint32_t status;
    uint32_t out = 0;
    if (file->listingIndex >= file->listing.size())
    {
        status = file->listing.empty() ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
    }
    else
    {
        const auto& entry = file->listing[file->listingIndex++];
        std::string name = entry.path().filename().string();
        uint32_t nameLength = std::min<uint32_t>(uint32_t(name.size()), length - 0x40);
        FileAttributes a{};
        StatPath(entry.path(), file->writable, a);

        memset(info, 0, length);
        info->nextEntryOffset = 0;
        info->fileIndex = uint32_t(file->listingIndex - 1);
        info->creationTime = a.creation;
        info->lastAccessTime = a.access;
        info->lastWriteTime = a.write;
        info->changeTime = a.write;
        info->endOfFile = a.size;
        info->allocationSize = (a.size + 0xFFF) & ~uint64_t(0xFFF);
        info->attributes = a.attributes;
        info->fileNameLength = nameLength;
        memcpy(info->fileName, name.data(), nameLength);
        status = X_STATUS_SUCCESS;
        out = 0x40 + nameLength;
    }

    SetIoStatus(iosb, status, out);
    SignalEvent(eventHandle);
    (void)apcRoutine; (void)apcContext;
    return status;
}

static uint32_t NtFlushBuffersFile(uint32_t fileHandle, XIO_STATUS_BLOCK* iosb)
{
    if (auto file = GetObjectAs<XFile>(fileHandle); file && file->fd >= 0)
        fsync(file->fd);
    SetIoStatus(iosb, X_STATUS_SUCCESS, 0);
    return X_STATUS_SUCCESS;
}

static uint32_t ObCreateSymbolicLink(XANSI_STRING* link, XANSI_STRING* device)
{
    std::string from = Lower(AnsiToString(link));
    std::string to = Lower(AnsiToString(device));
    if (from.rfind("\\??\\", 0) == 0)
        from = from.substr(4);
    fprintf(stderr, "[fs] link %s -> %s\n", from.c_str(), to.c_str());
    std::lock_guard lock(g_fsMutex);
    g_symlinks[from] = to;
    return X_STATUS_SUCCESS;
}

static uint32_t ObDeleteSymbolicLink(XANSI_STRING* link)
{
    std::string from = Lower(AnsiToString(link));
    if (from.rfind("\\??\\", 0) == 0)
        from = from.substr(4);
    std::lock_guard lock(g_fsMutex);
    g_symlinks.erase(from);
    return X_STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__NtCreateFile, NtCreateFile);
GUEST_FUNCTION_HOOK(__imp__NtOpenFile, NtOpenFile);
GUEST_FUNCTION_HOOK(__imp__NtReadFile, NtReadFile);
GUEST_FUNCTION_HOOK(__imp__NtReadFileScatter, NtReadFileScatter);
GUEST_FUNCTION_HOOK(__imp__NtWriteFile, NtWriteFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryInformationFile, NtQueryInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtSetInformationFile, NtSetInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryFullAttributesFile, NtQueryFullAttributesFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryVolumeInformationFile, NtQueryVolumeInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryDirectoryFile, NtQueryDirectoryFile);
GUEST_FUNCTION_HOOK(__imp__NtFlushBuffersFile, NtFlushBuffersFile);
GUEST_FUNCTION_HOOK(__imp__ObCreateSymbolicLink, ObCreateSymbolicLink);
GUEST_FUNCTION_HOOK(__imp__ObDeleteSymbolicLink, ObDeleteSymbolicLink);
