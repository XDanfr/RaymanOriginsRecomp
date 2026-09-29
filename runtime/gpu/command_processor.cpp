// GPU "nula" (#12, primeiro passo): processador de comandos PM4 que consome o
// ring buffer, executa os pacotes de sincronização (registradores, escritas em
// memória, fences, esperas, interrupções, buffers indiretos) e ignora os de
// desenho; gera vsync a 60 Hz chamando o callback de interrupção do jogo.
// Assim o jogo roda o loop principal; o desenho de verdade entra em cima disto.
//
// Semântica dos pacotes do Xenia (pm4_command_processor_implement.h,
// command_processor.cc, graphics_system.cc, xboxkrnl_video.cc).
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <thread>
#include <vector>
#include <set>
#include <filesystem>
#include <algorithm>
#include "cpu/guest_context.h"
#include "function.h"
#include "kernel/thread.h"
#include "kernel/video.h"
#include "memory.h"
#if defined(__SWITCH__)
#include "switch/presenter.h"
#endif

namespace
{
// Opcodes PM4 (tipo 3)
enum : uint32_t
{
    PM4_NOP = 0x10, PM4_REG_RMW = 0x21, PM4_DRAW_INDX = 0x22, PM4_WAIT_FOR_IDLE = 0x26,
    PM4_INDIRECT_BUFFER_PFD = 0x37, PM4_WAIT_REG_MEM = 0x3C, PM4_MEM_WRITE = 0x3D,
    PM4_REG_TO_MEM = 0x3E, PM4_INDIRECT_BUFFER = 0x3F, PM4_COND_WRITE = 0x45,
    PM4_EVENT_WRITE = 0x46, PM4_ME_INIT = 0x48, PM4_INTERRUPT = 0x54,
    PM4_EVENT_WRITE_SHD = 0x58, PM4_EVENT_WRITE_EXT = 0x5A, PM4_EVENT_WRITE_ZPD = 0x5B,
    PM4_XE_SWAP = 0x64, PM4_IM_LOAD = 0x27, PM4_IM_LOAD_IMMEDIATE = 0x2B, PM4_DRAW_INDX_2 = 0x36,
};

// Registradores
constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;
constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC;
constexpr uint32_t REG_SCRATCH_ADDR = 0x01DD;
constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;
constexpr uint32_t REG_WRITEBACK_START = 0x0A04;
constexpr uint32_t REG_WRITEBACK_SIZE = 0x0A05;
constexpr uint32_t REG_COHER_STATUS_HOST = 0x0A31;
constexpr uint32_t REG_VGT_EVENT_INITIATOR = 0x21F9;
constexpr uint32_t REG_RB_SAMPLE_COUNT_ADDR = 0x2325;
constexpr uint32_t REG_SHADER_CONSTANT_FETCH_00_0 = 0x4800;
constexpr uint32_t REGISTER_COUNT = 0x5003;

constexpr uint32_t MMIO_BASE = 0x7FC80000; // registradores da GPU no espaço do guest
constexpr uint32_t SWAP_SIGNATURE = 0x53574150; // "SWAP"

constexpr uint32_t MakePacketType0(uint32_t index, uint32_t count) { return ((count - 1) << 16) | index; }
constexpr uint32_t MakePacketType3(uint32_t opcode, uint32_t count) { return (3u << 30) | ((count - 1) << 16) | (opcode << 8); }

// Troca de bytes pedida pelos 2 bits baixos do endereço (xenos::Endian).
uint32_t GpuSwap(uint32_t value, uint32_t endian)
{
    switch (endian & 3)
    {
    case 1: return ((value >> 8) & 0x00FF00FF) | ((value << 8) & 0xFF00FF00); // 8in16
    case 2: return __builtin_bswap32(value);                                   // 8in32
    case 3: return (value >> 16) | (value << 16);                              // 16in32
    default: return value;
    }
}

uint32_t Load32(uint32_t guest) { return __builtin_bswap32(*static_cast<uint32_t*>(g_memory.Translate(guest))); }
void Store32(uint32_t guest, uint32_t value) { *static_cast<uint32_t*>(g_memory.Translate(guest)) = __builtin_bswap32(value); }

// Endereço da GPU (físico) -> endereço do guest onde a memória física vive.
uint32_t PhysicalToGuest(uint32_t address) { return 0xA0000000u + (address & 0x1FFFFFFFu); }

struct Gpu
{
    std::vector<uint32_t> registers = std::vector<uint32_t>(REGISTER_COUNT, 0);

    uint32_t ringBase = 0;          // endereço do guest
    uint32_t ringDwords = 0;
    uint32_t readIndex = 0;
    uint32_t readWriteback = 0;     // endereço físico
    uint32_t counter = 0;           // contador de vblank/swaps (EVENT_WRITE_SHD)
    uint64_t frames = 0;
    std::atomic<bool> running{ false };
};

Gpu g_gpu;
const bool g_gpuLog = getenv("RAYMAN_GPU_LOG") != nullptr;
int g_fenceLogs = 0, g_kickLogs = 0, g_packetLogs = 0;

void WriteRegister(uint32_t index, uint32_t value)
{
    if (index >= REGISTER_COUNT)
        return;
    if (index == REG_COHER_STATUS_HOST)
        value &= ~0x80000000u; // coerência de cache "concluída" na hora
    g_gpu.registers[index] = value;

    // Registradores scratch: com o bit ligado em SCRATCH_UMSK, o hardware espelha
    // o valor em SCRATCH_ADDR + n*4. O D3D usa isso para sincronizar CPU e GPU.
    if (index >= REG_SCRATCH_REG0 && index < REG_SCRATCH_REG0 + 8)
    {
        uint32_t n = index - REG_SCRATCH_REG0;
        if ((1u << n) & g_gpu.registers[REG_SCRATCH_UMSK])
            Store32(PhysicalToGuest(g_gpu.registers[REG_SCRATCH_ADDR] + n * 4), value);
    }
}

// Contexto do guest da thread que dispara a interrupção (GPU ou vsync).
thread_local PPCContext* t_interruptCtx = nullptr;
std::mutex g_interruptMutex; // o callback do jogo não espera ser reentrante

void DispatchInterrupt(uint32_t source, uint32_t cpu)
{
    uint32_t callback = g_graphicsInterrupt.callback.load();
    if (callback == 0 || t_interruptCtx == nullptr)
        return;
    std::lock_guard lock(g_interruptMutex);
    PPCContext& ctx = *t_interruptCtx;
    ctx.r3.u64 = source;
    ctx.r4.u64 = g_graphicsInterrupt.userData.load();
    // O tratador lê a CPU atual em PCR+0x10C e confirma a interrupção limpando o bit
    // dela numa máscara que a GPU espera zerar: roda "como" a CPU pedida (Xenia).
    *static_cast<uint8_t*>(g_memory.Translate(ctx.r13.u32 + 0x10C)) = uint8_t(cpu);
    PPCFunc* function = g_memory.FindFunction(callback);
    if (function == nullptr)
    {
        fprintf(stderr, "[gpu] interrupção callback 0x%08X sem função recompilada\n", callback);
        return;
    }
    function(ctx, g_memory.base);
}


// ---- Estatísticas (RAYMAN_GPU_STATS) e dump de shaders para private/shaders ----
// Os shaders são código do jogo: ficam só na pasta local do usuário (fora do git).
struct GpuStats
{
    uint64_t draws = 0, drawsThisFrame = 0, maxDrawsPerFrame = 0, shaderLoads = 0;
    std::set<uint64_t> vertexShaders, pixelShaders;
    std::set<uint32_t> colorInfos, depthInfos;
};
GpuStats g_stats;
const bool g_gpuStats = getenv("RAYMAN_GPU_STATS") != nullptr;
constexpr uint32_t REG_RB_COLOR_INFO = 0x2001;
constexpr uint32_t REG_RB_DEPTH_INFO = 0x2002;

void RecordShader(uint32_t type, const uint32_t* words, uint32_t dwords, bool swapped)
{
    g_stats.shaderLoads++;
    std::vector<uint32_t> code(dwords);
    for (uint32_t i = 0; i < dwords; i++)
        code[i] = swapped ? __builtin_bswap32(words[i]) : words[i];
    uint64_t hash = 0xCBF29CE484222325ull; // FNV-1a
    for (uint32_t w : code)
        for (int b = 0; b < 4; b++)
            hash = (hash ^ ((w >> (b * 8)) & 0xFF)) * 0x100000001B3ull;
    auto& set = type == 0 ? g_stats.vertexShaders : g_stats.pixelShaders;
    if (!set.insert(hash).second || !g_gpuStats)
        return;
    std::filesystem::create_directories("private/shaders");
    char name[96];
    snprintf(name, sizeof(name), "private/shaders/%s_%016llx.bin", type == 0 ? "vs" : "ps", (unsigned long long)hash);
    if (FILE* f = fopen(name, "wb"))
    {
        // Big-endian, como na memória do Xbox.
        for (uint32_t w : code)
        {
            uint32_t be = __builtin_bswap32(w);
            fwrite(&be, 4, 1, f);
        }
        fclose(f);
    }
}

void RecordDraw()
{
    g_stats.draws++;
    g_stats.drawsThisFrame++;
    g_stats.colorInfos.insert(g_gpu.registers[REG_RB_COLOR_INFO]);
    g_stats.depthInfos.insert(g_gpu.registers[REG_RB_DEPTH_INFO]);
}

void RecordFrame(uint64_t frame)
{
    g_stats.maxDrawsPerFrame = std::max(g_stats.maxDrawsPerFrame, g_stats.drawsThisFrame);
    g_stats.drawsThisFrame = 0;
    if (!g_gpuStats || frame % 120 != 0)
        return;
    fprintf(stderr, "[gpu-stats] frame %llu: %llu desenhos (max %llu/frame), %llu cargas de shader, %zu VS e %zu PS distintos, %zu formatos de cor, %zu de profundidade\n",
            (unsigned long long)frame, (unsigned long long)g_stats.draws, (unsigned long long)g_stats.maxDrawsPerFrame,
            (unsigned long long)g_stats.shaderLoads, g_stats.vertexShaders.size(), g_stats.pixelShaders.size(),
            g_stats.colorInfos.size(), g_stats.depthInfos.size());
}

bool MatchValue(uint32_t value, uint32_t ref, uint32_t waitInfo)
{
    return ((((value < ref) << 1) | ((value <= ref) << 2) | ((value == ref) << 3) | ((value != ref) << 4) |
             ((value >= ref) << 5) | ((value > ref) << 6) | (1 << 7)) >> (waitInfo & 7)) & 1;
}

// Leitor de dwords: ring (circular) ou buffer linear (indirect buffer).
struct Reader
{
    uint32_t base, sizeDwords, index;
    bool ring;

    uint32_t Read()
    {
        uint32_t value = Load32(base + (index % sizeDwords) * 4);
        index = ring ? (index + 1) % sizeDwords : index + 1;
        return value;
    }
    void Skip(uint32_t dwords) { index = ring ? (index + dwords) % sizeDwords : index + dwords; }
};

void ExecuteBuffer(Reader& reader, uint32_t endIndex);

void ExecutePacket(Reader& reader)
{
    uint32_t packetAddress = reader.base + (reader.index % reader.sizeDwords) * 4;
    uint32_t packet = reader.Read();
    if (g_gpuLog && g_packetLogs++ < 2000)
        fprintf(stderr, "[gpu]   pacote @0x%08X: 0x%08X (tipo %u, op 0x%02X, n %u)\n", packetAddress, packet, packet >> 30, (packet >> 8) & 0x7F, ((packet >> 16) & 0x3FFF) + 1);
    if (packet == 0 || packet == 0x0BADF00D)
        return;

    switch (packet >> 30)
    {
    case 0:
    {
        uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
        uint32_t base = packet & 0x7FFF;
        bool oneRegister = (packet >> 15) & 1;
        for (uint32_t i = 0; i < count; i++)
            WriteRegister(oneRegister ? base : base + i, reader.Read());
        return;
    }
    case 1:
    {
        uint32_t a = reader.Read(), b = reader.Read();
        WriteRegister(packet & 0x7FF, a);
        WriteRegister((packet >> 11) & 0x7FF, b);
        return;
    }
    case 2:
        return;
    }

    uint32_t opcode = (packet >> 8) & 0x7F;
    uint32_t count = ((packet >> 16) & 0x3FFF) + 1;

    switch (opcode)
    {
    case PM4_INTERRUPT:
    {
        uint32_t cpuMask = reader.Read();
        for (uint32_t n = 0; n < 6; n++)
            if (cpuMask & (1u << n))
                DispatchInterrupt(1, n);
        reader.Skip(count - 1);
        return;
    }
    case PM4_XE_SWAP:
    {
        reader.Read(); // assinatura
        uint32_t frontBuffer = reader.Read();
        uint32_t width = reader.Read(), height = reader.Read();
        reader.Skip(count - 4);
        g_gpu.counter++;
        RecordFrame(g_gpu.frames + 1);
        ++g_gpu.frames;
#if defined(__SWITCH__)
        SubmitSwitchPresentationFrame(g_gpu.frames, frontBuffer, width, height);
#endif
        if (g_gpu.frames <= 3 || (g_gpu.frames % 300) == 0)
            fprintf(stderr, "[gpu] frame %llu: front buffer 0x%08X %ux%u\n",
                    (unsigned long long)g_gpu.frames, frontBuffer, width, height);
        return;
    }
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
    {
        uint32_t address = reader.Read();
        uint32_t length = reader.Read() & 0xFFFFF;
        reader.Skip(count - 2);
        Reader sub{ PhysicalToGuest(address), length ? length : 1, 0, false };
        ExecuteBuffer(sub, length);
        return;
    }
    case PM4_WAIT_REG_MEM:
    {
        uint32_t waitInfo = reader.Read(), pollAddress = reader.Read();
        uint32_t ref = reader.Read(), mask = reader.Read(), wait = reader.Read();
        reader.Skip(count - 5);
        bool memory = waitInfo & 0x10;
        if (g_gpuLog)
            fprintf(stderr, "[gpu] WAIT_REG_MEM info=0x%X %s=0x%08X ref=0x%08X mask=0x%08X espera=0x%X\n", waitInfo, memory ? "mem" : "reg", pollAddress, ref, mask, wait);
        for (int attempt = 0;; attempt++)
        {
            uint32_t value = memory ? GpuSwap(*static_cast<uint32_t*>(g_memory.Translate(PhysicalToGuest(pollAddress & ~3u))), pollAddress & 3)
                                    : g_gpu.registers[pollAddress % REGISTER_COUNT];
            if (MatchValue(value & mask, ref, waitInfo))
                break;
            // Registradores que só o hardware mudaria: não trava o jogo esperando.
            if (!memory || !g_gpu.running)
                break;
            std::this_thread::sleep_for(std::chrono::microseconds(wait >= 0x100 ? 1000 : 100));
        }
        return;
    }
    case PM4_REG_RMW:
    {
        uint32_t info = reader.Read(), andMask = reader.Read(), orMask = reader.Read();
        reader.Skip(count - 3);
        uint32_t value = g_gpu.registers[info & 0x1FFF];
        value &= (info >> 31) & 1 ? g_gpu.registers[andMask & 0x1FFF] : andMask;
        value |= (info >> 30) & 1 ? g_gpu.registers[orMask & 0x1FFF] : orMask;
        WriteRegister(info & 0x1FFF, value);
        return;
    }
    case PM4_REG_TO_MEM:
    {
        uint32_t reg = reader.Read(), address = reader.Read();
        reader.Skip(count - 2);
        uint32_t value = GpuSwap(g_gpu.registers[reg % REGISTER_COUNT], address & 3);
        *static_cast<uint32_t*>(g_memory.Translate(PhysicalToGuest(address & ~3u))) = value;
        return;
    }
    case PM4_MEM_WRITE:
    {
        uint32_t address = reader.Read();
        for (uint32_t i = 0; i < count - 1; i++, address += 4)
            *static_cast<uint32_t*>(g_memory.Translate(PhysicalToGuest(address & ~3u))) = GpuSwap(reader.Read(), address & 3);
        return;
    }
    case PM4_COND_WRITE:
    {
        uint32_t waitInfo = reader.Read(), pollAddress = reader.Read(), ref = reader.Read();
        uint32_t mask = reader.Read(), writeAddress = reader.Read(), data = reader.Read();
        reader.Skip(count - 6);
        uint32_t value = (waitInfo & 0x10)
            ? GpuSwap(*static_cast<uint32_t*>(g_memory.Translate(PhysicalToGuest(pollAddress & ~3u))), pollAddress & 3)
            : g_gpu.registers[pollAddress % REGISTER_COUNT];
        if (MatchValue(value & mask, ref, waitInfo))
        {
            if (waitInfo & 0x100)
                *static_cast<uint32_t*>(g_memory.Translate(PhysicalToGuest(writeAddress & ~3u))) = GpuSwap(data, writeAddress & 3);
            else
                WriteRegister(writeAddress, data);
        }
        return;
    }
    case PM4_EVENT_WRITE:
        WriteRegister(REG_VGT_EVENT_INITIATOR, reader.Read() & 0x3F);
        reader.Skip(count - 1);
        return;
    case PM4_EVENT_WRITE_SHD:
    {
        // Fence: grava um valor (ou o contador) quando o trabalho anterior termina.
        uint32_t initiator = reader.Read(), address = reader.Read(), value = reader.Read();
        reader.Skip(count - 3);
        WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        uint32_t data = GpuSwap((initiator >> 31) & 1 ? g_gpu.counter : value, address & 3);
        uint32_t target = address & ~3u;
        uint32_t destination = PhysicalToGuest(target);
        if (target > 0x1FFFFFFF)
        {
            uint32_t base = g_gpu.registers[REG_WRITEBACK_START], size = g_gpu.registers[REG_WRITEBACK_SIZE];
            if (base != 0 && target - base < size)
                destination = 0x7F000000 + (target - base);
        }
        *static_cast<uint32_t*>(g_memory.Translate(destination)) = data;
        if (g_gpuLog && g_fenceLogs++ < 8)
            fprintf(stderr, "[gpu] fence: 0x%08X -> guest 0x%08X (endian %u)\n", (initiator >> 31) & 1 ? g_gpu.counter : value, destination, address & 3);
        return;
    }
    case PM4_EVENT_WRITE_EXT:
    {
        // Extensão de tela do último desenho: "tudo" (Xenia).
        uint32_t initiator = reader.Read(), address = reader.Read();
        reader.Skip(count - 2);
        WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        uint16_t extents[] = { 0, uint16_t(8192 >> 3), 0, uint16_t(8192 >> 3), 0, 1 };
        auto* destination = static_cast<uint16_t*>(g_memory.Translate(PhysicalToGuest(address & ~3u)));
        for (int i = 0; i < 6; i++)
            destination[i] = __builtin_bswap16(extents[i]);
        return;
    }
    case PM4_IM_LOAD:
    {
        uint32_t addressType = reader.Read(), startSize = reader.Read();
        reader.Skip(count - 2);
        RecordShader(addressType & 3, static_cast<const uint32_t*>(g_memory.Translate(PhysicalToGuest(addressType & ~3u))), startSize & 0xFFFF, true);
        return;
    }
    case PM4_IM_LOAD_IMMEDIATE:
    {
        uint32_t type = reader.Read(), startSize = reader.Read();
        uint32_t dwords = std::min(startSize & 0xFFFF, count - 2);
        std::vector<uint32_t> code(dwords);
        for (uint32_t i = 0; i < dwords; i++)
            code[i] = reader.Read();
        reader.Skip(count - 2 - dwords);
        RecordShader(type & 3, code.data(), dwords, false);
        return;
    }
    case PM4_DRAW_INDX:
    case PM4_DRAW_INDX_2:
        RecordDraw();
        reader.Skip(count);
        return;
    case PM4_EVENT_WRITE_ZPD:
        // Consultas de oclusão: sem relatório por enquanto.
        WriteRegister(REG_VGT_EVENT_INITIATOR, reader.Read() & 0x3F);
        reader.Skip(count - 1);
        return;
    default:
        // ME_INIT, NOP, desenhos, constantes, shaders...: ignorados nesta GPU nula.
        reader.Skip(count);
        return;
    }
}

void ExecuteBuffer(Reader& reader, uint32_t endIndex)
{
    while (reader.index != endIndex && g_gpu.running)
        ExecutePacket(reader);
}

void GpuThread()
{
    PPCContext ctx;
    if (InitMainThread(ctx) == nullptr)
    {
        fprintf(stderr, "[gpu] não consegui inicializar o contexto da thread GPU\n");
        return;
    }
    // bloco de thread do guest para rodar o callback de interrupção
    t_interruptCtx = &ctx;

    while (g_gpu.running)
    {
        uint32_t writeIndex = Load32(MMIO_BASE + REG_CP_RB_WPTR * 4) % (g_gpu.ringDwords ? g_gpu.ringDwords : 1);
        if (g_gpu.ringDwords && g_gpu.readIndex != writeIndex)
        {
            if (g_gpuLog && g_kickLogs++ < 8)
                fprintf(stderr, "[gpu] kick: leitura %u -> escrita %u\n", g_gpu.readIndex, writeIndex);
            Reader ring{ g_gpu.ringBase, g_gpu.ringDwords, g_gpu.readIndex, true };
            ExecuteBuffer(ring, writeIndex);
            g_gpu.readIndex = ring.index;
            if (g_gpu.readWriteback)
                Store32(PhysicalToGuest(g_gpu.readWriteback), g_gpu.readIndex);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

// Vsync numa thread própria (como o Xenia): o processador de comandos pode ficar
// preso num WAIT_REG_MEM esperando justamente o que o tratador de vblank escreve.
void VsyncThread()
{
    PPCContext ctx;
    if (InitMainThread(ctx) == nullptr)
    {
        fprintf(stderr, "[gpu] não consegui inicializar o contexto da thread vsync\n");
        return;
    }
    t_interruptCtx = &ctx;

    auto next = std::chrono::steady_clock::now();
    while (g_gpu.running)
    {
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        g_gpu.counter++;
        DispatchInterrupt(0, 2); // vblank
    }
}

#if defined(__SWITCH__)
void* GpuThreadEntry(void*)
{
    try
    {
        GpuThread();
    }
    catch (...)
    {
        fprintf(stderr, "[gpu] worker terminou por exceção não tratada\n");
        g_gpu.running = false;
        ClearPPCContext();
    }
    return nullptr;
}

void* VsyncThreadEntry(void*)
{
    try
    {
        VsyncThread();
    }
    catch (...)
    {
        fprintf(stderr, "[gpu] vsync terminou por exceção não tratada\n");
        g_gpu.running = false;
        ClearPPCContext();
    }
    return nullptr;
}

bool StartSwitchWorker(void* (*entry)(void*), const char* name)
{
    pthread_t thread;
    const int createResult = pthread_create(&thread, nullptr, entry, nullptr);
    if (createResult != 0)
    {
        fprintf(stderr, "[gpu] pthread_create(%s) falhou (%d)\n", name, createResult);
        return false;
    }

    const int detachResult = pthread_detach(thread);
    // Current devkitA64/libnx has no Horizon thread-detach syscall. ENOSYS is
    // therefore expected; the process-lifetime GPU workers remain joinable.
    if (detachResult != 0 && detachResult != ENOSYS)
    {
        fprintf(stderr, "[gpu] pthread_detach(%s) falhou (%d)\n", name, detachResult);
        g_gpu.running = false;
        return false;
    }
    if (detachResult == ENOSYS)
        fprintf(stderr, "[gpu] pthread_detach(%s) indisponível; worker mantido\n", name);
    return true;
}
#endif

void StartGpu()
{
    if (g_gpu.running.exchange(true))
        return;

    // The desktop backend's flat mmap made the Xenos register window appear
    // implicitly available. Switch guest memory is sparse, so back the whole
    // register range before the first startup writes below.
    if (!g_memory.CommitRange(MMIO_BASE, REGISTER_COUNT * sizeof(uint32_t)))
    {
        g_gpu.running = false;
        fprintf(stderr, "[gpu] não consegui mapear a janela MMIO 0x%08X..0x%08X\n",
                MMIO_BASE, MMIO_BASE + REGISTER_COUNT * sizeof(uint32_t));
        return;
    }

    // Registradores lidos por MMIO que o jogo espera ver preenchidos (Xenia).
    Store32(MMIO_BASE + 0x0F00 * 4, 0x08100748); // RB_EDRAM_TIMING
    Store32(MMIO_BASE + 0x0F01 * 4, 0x0000200E); // RB_BC_CONTROL
    Store32(MMIO_BASE + 0x1951 * 4, 0x00000001); // status de interrupção: vblank
    Store32(MMIO_BASE + 0x1961 * 4, 0x050002D0); // AVIVO_D1MODE_VIEWPORT_SIZE: 1280x720

#if defined(__SWITCH__)
    if (!StartSwitchWorker(GpuThreadEntry, "gpu") ||
        !StartSwitchWorker(VsyncThreadEntry, "vsync"))
    {
        g_gpu.running = false;
        return;
    }
#else
    std::thread(GpuThread).detach();
    std::thread(VsyncThread).detach();
#endif
    fprintf(stderr, "[gpu] processador de comandos iniciado (ring 0x%08X, %u dwords)\n", g_gpu.ringBase, g_gpu.ringDwords);
}
} // namespace

// ---- Imports ----

static void VdInitializeRingBuffer(uint32_t pointer, uint32_t sizeLog2)
{
    g_gpu.ringBase = PhysicalToGuest(pointer);
    g_gpu.ringDwords = (1u << (sizeLog2 + 3)) / 4;
    g_gpu.readIndex = 0;
    memset(g_memory.Translate(g_gpu.ringBase), 0, g_gpu.ringDwords * 4);
    StartGpu();
}

static void VdEnableRingBufferRPtrWriteBack(uint32_t pointer, uint32_t blockSizeLog2)
{
    g_gpu.readWriteback = pointer;
    (void)blockSizeLog2;
}

static void VdSetSystemCommandBufferGpuIdentifierAddress(uint32_t unknown) { (void)unknown; }

struct X_DISPLAY_INFO
{
    be<uint16_t> frontBufferWidth, frontBufferHeight;
    uint8_t frontBufferColorFormat, frontBufferPixelFormat;
    uint8_t pad0[2];
    be<uint32_t> scalerSourceX1, scalerSourceY1, scalerSourceX2, scalerSourceY2;
    be<uint32_t> scaledOutputWidth, scaledOutputHeight;
    be<uint32_t> verticalFilterType;
    uint8_t verticalFilterParams[12];
    be<uint32_t> horizontalFilterType;
    uint8_t horizontalFilterParams[12];
    be<uint16_t> overscanLeft, overscanTop, overscanRight, overscanBottom;
    be<uint16_t> displayWidth, displayHeight;
    be<float> displayRefreshRate;
    be<uint32_t> displayInterlaced;
    uint8_t displayColorFormat;
    uint8_t pad1;
    be<uint16_t> actualDisplayWidth;
};
static_assert(sizeof(X_DISPLAY_INFO) == 0x58);

static void VdGetCurrentDisplayInformation(X_DISPLAY_INFO* info)
{
    memset(info, 0, sizeof(*info));
    info->frontBufferWidth = 1280;
    info->frontBufferHeight = 720;
    info->scalerSourceX2 = 1280;
    info->scalerSourceY2 = 720;
    info->scaledOutputWidth = 1280;
    info->scaledOutputHeight = 720;
    info->verticalFilterType = 1;
    info->horizontalFilterType = 1;
    info->overscanLeft = 320;
    info->overscanTop = 180;
    info->overscanRight = 320;
    info->overscanBottom = 180;
    info->displayWidth = 1280;
    info->displayHeight = 720;
    info->displayRefreshRate = 60.0f;
    info->actualDisplayWidth = 1280;
}

// Preenche o buffer do scaler com NOPs (tipo 2); o jogo só confere o sucesso (Xenia).
static uint32_t VdInitializeScalerCommandBuffer(uint32_t sourceXY, uint32_t sourceWH, uint32_t outputXY, uint32_t outputWH,
                                                uint32_t frontBufferWH, uint32_t verticalFilter, uint32_t verticalParams,
                                                uint32_t horizontalFilter, uint32_t horizontalParams, uint32_t unknown,
                                                be<uint32_t>* destination, uint32_t destinationCount)
{
    (void)sourceXY; (void)sourceWH; (void)outputXY; (void)outputWH; (void)frontBufferWH;
    (void)verticalFilter; (void)verticalParams; (void)horizontalFilter; (void)horizontalParams; (void)unknown;
    for (uint32_t i = 0; destination && i < destinationCount; i++)
        destination[i] = 0x80000000u;
    return 1;
}

// Fim de frame: escreve no ring o fetch do front buffer e um pacote XE_SWAP (como o Xenia).
static void VdSwap(be<uint32_t>* buffer, be<uint32_t>* fetch, uint32_t unknown2, uint32_t unknown3, uint32_t unknown4,
                   be<uint32_t>* frontBufferPtr, be<uint32_t>* textureFormat, be<uint32_t>* colorSpace,
                   be<uint32_t>* width, be<uint32_t>* height)
{
    (void)unknown2; (void)unknown3; (void)unknown4; (void)textureFormat; (void)colorSpace;
    uint32_t fetchWords[6];
    for (int i = 0; i < 6; i++)
        fetchWords[i] = fetch[i].get();
    // base_address (dword 1, bits 12..31) no header da textura é virtual: converte para físico.
    uint32_t frontBufferVirtual = fetchWords[1] & 0xFFFFF000u;
    uint32_t frontBufferPhysical = frontBufferVirtual & 0x1FFFFFFFu;
    fetchWords[1] = (fetchWords[1] & 0xFFFu) | frontBufferPhysical;
    (void)frontBufferPtr;

    for (int i = 0; i < 64; i++)
        buffer[i] = 0u;
    uint32_t offset = 0;
    buffer[offset++] = MakePacketType0(REG_SHADER_CONSTANT_FETCH_00_0, 6);
    for (int i = 0; i < 6; i++)
        buffer[offset++] = fetchWords[i];
    buffer[offset++] = MakePacketType3(PM4_XE_SWAP, 4);
    buffer[offset++] = SWAP_SIGNATURE;
    buffer[offset++] = frontBufferPhysical;
    buffer[offset++] = width ? width->get() : 1280u;
    buffer[offset++] = height ? height->get() : 720u;
}

GUEST_FUNCTION_HOOK(__imp__VdInitializeRingBuffer, VdInitializeRingBuffer);
GUEST_FUNCTION_HOOK(__imp__VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack);
GUEST_FUNCTION_HOOK(__imp__VdSetSystemCommandBufferGpuIdentifierAddress, VdSetSystemCommandBufferGpuIdentifierAddress);
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayInformation, VdGetCurrentDisplayInformation);
GUEST_FUNCTION_HOOK(__imp__VdInitializeScalerCommandBuffer, VdInitializeScalerCommandBuffer);
GUEST_FUNCTION_HOOK(__imp__VdSwap, VdSwap);
