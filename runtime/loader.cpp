// Só este arquivo inclui a XenonUtils: o resto do runtime usa os headers gerados.
#include "loader.h"
#include <cstdio>
#include <cstring>
#include "memory.h"
#include <file.h>
#include <image.h>

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
