// Só este arquivo inclui a XenonUtils: o resto do runtime usa os headers gerados.
#include "loader.h"
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "kernel/heap.h"
#include "memory.h"
#include <file.h>
#include <image.h>
#include <xex.h>

namespace
{
struct ExportInfo
{
    uint32_t ordinal;
    const char* name;
    bool variable;
};

#define kFunction false
#define kVariable true
#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) { ORDINAL, #NAME, TYPE }
constexpr ExportInfo XboxKernelExports[] = {
#include <xbox/xboxkrnl_table.inc>
};
constexpr ExportInfo XamExports[] = {
#include <xbox/xam_table.inc>
};
#undef XE_EXPORT
#undef kVariable
#undef kFunction

const ExportInfo* FindExport(std::string_view library, uint32_t ordinal)
{
    const ExportInfo* exports = nullptr;
    size_t count = 0;
    if (library == "xboxkrnl.exe")
    {
        exports = XboxKernelExports;
        count = std::size(XboxKernelExports);
    }
    else if (library == "xam.xex")
    {
        exports = XamExports;
        count = std::size(XamExports);
    }

    for (size_t i = 0; i < count; ++i)
        if (exports[i].ordinal == ordinal)
            return &exports[i];
    return nullptr;
}

// XenonRecomp replaces function thunks in the decoded image, but variable
// imports remain ordinal tokens until the platform loader assigns guest
// storage to them. A fully mapped desktop guest arena happened to make those
// byte-swapped tokens read as zero. Sparse Switch memory exposes the missing
// relocation immediately, so resolve every imported variable explicitly.
bool ResolveVariableImports(
    const uint8_t* xexData,
    Image& image,
    uint8_t* guestBase)
{
    const auto* imports = reinterpret_cast<const Xex2ImportHeader*>(
        getOptHeaderPtr(xexData, XEX_HEADER_IMPORT_LIBRARIES));
    if (imports == nullptr)
        return true;

    std::vector<std::string_view> libraryNames;
    const char* stringTable = reinterpret_cast<const char*>(imports + 1);
    size_t stringOffset = 0;
    for (size_t i = 0; i < imports->numImports; ++i)
    {
        libraryNames.emplace_back(stringTable + stringOffset);
        stringOffset += (libraryNames.back().size() + 4) & ~size_t(3);
    }

    auto* library = reinterpret_cast<const Xex2ImportLibrary*>(
        reinterpret_cast<const uint8_t*>(imports + 1) +
        static_cast<uint32_t>(imports->sizeOfStringTable));
    std::unordered_map<uint64_t, uint32_t> resolvedVariables;
    size_t importEntries = 0;
    size_t recognizedVariables = 0;
    size_t unrecognizedThunkTokens = 0;

    for (std::string_view libraryName : libraryNames)
    {
        const auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);
        for (size_t i = 0; i < library->numberOfImports; ++i)
        {
            ++importEntries;
            const uint32_t thunkAddress = descriptors[i].firstThunk;
            if (thunkAddress < image.base ||
                thunkAddress + sizeof(uint32_t) > image.base + image.size)
            {
                fprintf(stderr, "[loader] import thunk fora da imagem: 0x%08X\n", thunkAddress);
                return false;
            }

            uint8_t* thunk = image.data.get() + (thunkAddress - image.base);
            uint32_t decodedImport = 0;
            memcpy(&decodedImport, thunk, sizeof(decodedImport));
            const uint32_t ordinal = decodedImport & 0xFFFFu;
            const ExportInfo* exportInfo = FindExport(libraryName, ordinal);
            if (exportInfo == nullptr || !exportInfo->variable)
            {
                if (exportInfo == nullptr)
                    ++unrecognizedThunkTokens;
                continue;
            }

            ++recognizedVariables;

            const uint64_t key = (uint64_t(std::hash<std::string_view>{}(libraryName)) << 32) |
                                 ordinal;
            auto [it, inserted] = resolvedVariables.emplace(key, 0);
            if (inserted)
            {
                constexpr size_t VARIABLE_STORAGE_SIZE = 0x100;
                void* storage = g_runtimeHeap.AllocZeroed(VARIABLE_STORAGE_SIZE);
                if (storage == nullptr)
                {
                    fprintf(stderr, "[loader] sem memória para variável importada %s!%s\n",
                            std::string(libraryName).c_str(), exportInfo->name);
                    return false;
                }
                it->second = static_cast<uint32_t>(
                    static_cast<uint8_t*>(storage) - guestBase);
                fprintf(stderr, "[loader] variável %s!%s -> 0x%08X\n",
                        std::string(libraryName).c_str(), exportInfo->name, it->second);
            }

            be<uint32_t> relocatedAddress = it->second;
            memcpy(thunk, &relocatedAddress, sizeof(relocatedAddress));
        }

        library = reinterpret_cast<const Xex2ImportLibrary*>(
            reinterpret_cast<const uint8_t*>(library) +
            static_cast<uint32_t>(library->size));
    }

    fprintf(stderr,
            "[loader] imports: %zu entries, %zu variables relocated, %zu non-variable thunk tokens\n",
            importEntries, recognizedVariables, unrecognizedThunkTokens);
    return true;
}
}

bool LoadXexImage(const char* path, uint8_t* guestBase, LoadedImage& out)
{
    auto file = LoadFile(path);
    if (file.size() < 4)
    {
        fprintf(stderr, "[loader] não consegui ler %s\n", path);
        return false;
    }

    auto image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr || image.size == 0)
    {
        fprintf(stderr, "[loader] XEX inválido: %s\n", path);
        return false;
    }

    if (!g_runtimeHeap.IsInitialized())
    {
        fprintf(stderr, "[loader] heap do runtime ainda não foi inicializado\n");
        return false;
    }

    if (!ResolveVariableImports(file.data(), image, guestBase))
        return false;

    if (image.base > UINT32_MAX ||
        image.size > PPC_MEMORY_SIZE - static_cast<uint32_t>(image.base))
    {
        fprintf(stderr, "[loader] imagem fora do espaço do guest: %s\n", path);
        return false;
    }

    if (!g_memory.CommitRange(static_cast<uint32_t>(image.base), image.size))
    {
        fprintf(stderr, "[loader] não consegui mapear a imagem no guest: %s\n", path);
        return false;
    }

    memcpy(guestBase + image.base, image.data.get(), image.size);

    out.base = uint32_t(image.base);
    out.size = image.size;
    out.entryPoint = uint32_t(image.entry_point);

    fprintf(stderr, "[loader] imagem 0x%08X..0x%08X, entry 0x%08X\n",
            out.base, out.base + out.size, out.entryPoint);
    return true;
}
