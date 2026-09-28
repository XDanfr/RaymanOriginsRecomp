// Switch-local in-memory LZX adapter.  XenonUtils' equivalent lives in
// xex_patcher.cpp alongside desktop-only memory-mapped patch-file support.
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/types.h>

#include <lzx.h>
#include <mspack.h>

namespace
{
struct MemoryFile
{
    mspack_system sys;
    void* buffer;
    size_t size;
    size_t offset;
};

MemoryFile* OpenMemoryFile(void* buffer, size_t size)
{
    if (buffer == nullptr || size >= INT_MAX)
        return nullptr;

    auto* file = static_cast<MemoryFile*>(std::calloc(1, sizeof(MemoryFile)));
    if (file != nullptr)
    {
        file->buffer = buffer;
        file->size = size;
    }
    return file;
}

void CloseMemoryFile(MemoryFile* file)
{
    std::free(file);
}

int ReadMemoryFile(mspack_file* file, void* buffer, int chars)
{
    if (chars <= 0)
        return 0;

    auto* memoryFile = reinterpret_cast<MemoryFile*>(file);
    const size_t count = std::min<size_t>(chars, memoryFile->size - memoryFile->offset);
    std::memcpy(buffer, static_cast<uint8_t*>(memoryFile->buffer) + memoryFile->offset, count);
    memoryFile->offset += count;
    return static_cast<int>(count);
}

int WriteMemoryFile(mspack_file* file, void* buffer, int chars)
{
    if (chars <= 0)
        return 0;

    auto* memoryFile = reinterpret_cast<MemoryFile*>(file);
    const size_t count = std::min<size_t>(chars, memoryFile->size - memoryFile->offset);
    std::memcpy(static_cast<uint8_t*>(memoryFile->buffer) + memoryFile->offset, buffer, count);
    memoryFile->offset += count;
    return static_cast<int>(count);
}

void* AllocateMemory(mspack_system*, size_t size)
{
    return std::calloc(size, 1);
}

void FreeMemory(void* memory)
{
    std::free(memory);
}

void CopyMemory(void* source, void* destination, size_t size)
{
    std::memcpy(destination, source, size);
}

mspack_system* CreateMemorySystem()
{
    auto* system = static_cast<mspack_system*>(std::calloc(1, sizeof(mspack_system)));
    if (system != nullptr)
    {
        system->read = ReadMemoryFile;
        system->write = WriteMemoryFile;
        system->alloc = AllocateMemory;
        system->free = FreeMemory;
        system->copy = CopyMemory;
    }
    return system;
}

bool GetWindowBits(uint32_t windowSize, uint32_t& windowBits)
{
    if (windowSize == 0 || (windowSize & (windowSize - 1)) != 0)
        return false;

    windowBits = static_cast<uint32_t>(__builtin_ctz(windowSize));
    return true;
}
}

int lzxDecompress(const void* source, size_t sourceSize, void* destination,
                  size_t destinationSize, uint32_t windowSize,
                  void* windowData, size_t windowDataSize)
{
    uint32_t windowBits = 0;
    if (source == nullptr || destination == nullptr || sourceSize >= INT_MAX ||
        destinationSize >= INT_MAX || windowDataSize > windowSize ||
        !GetWindowBits(windowSize, windowBits))
    {
        return 1;
    }

    mspack_system* system = CreateMemorySystem();
    if (system == nullptr)
        return 1;

    MemoryFile* input = OpenMemoryFile(const_cast<void*>(source), sourceSize);
    MemoryFile* output = OpenMemoryFile(destination, destinationSize);
    int result = 1;

    if (input != nullptr && output != nullptr)
    {
        lzxd_stream* stream = lzxd_init(
            system,
            reinterpret_cast<mspack_file*>(input),
            reinterpret_cast<mspack_file*>(output),
            windowBits, 0, 0x8000, destinationSize, 0);
        if (stream != nullptr)
        {
            if (windowData != nullptr)
            {
                const size_t padding = windowSize - windowDataSize;
                std::memset(stream->window, 0, padding);
                std::memcpy(stream->window + padding, windowData, windowDataSize);
                stream->ref_data_size = windowSize;
            }
            result = lzxd_decompress(stream, destinationSize);
            lzxd_free(stream);
        }
    }

    if (input != nullptr)
        CloseMemoryFile(input);
    if (output != nullptr)
        CloseMemoryFile(output);
    std::free(system);
    return result;
}
