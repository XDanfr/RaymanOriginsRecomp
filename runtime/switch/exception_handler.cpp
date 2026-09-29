#include <cstdint>
#include <cstdio>

#include <switch.h>

#include "cpu/guest_context.h"
#include "memory.h"

extern "C" void _start();

extern "C"
{
alignas(16) uint8_t __nx_exception_stack[0x4000];
uint64_t __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump* dump)
{
    FILE* file = fopen("sdmc:/switch/RaymanOriginsRecomp/crash.log", "w");
    if (file == nullptr)
        return;

    const uintptr_t imageBase = reinterpret_cast<uintptr_t>(&_start);
    const uintptr_t pc = static_cast<uintptr_t>(dump->pc.x);
    const uintptr_t far = static_cast<uintptr_t>(dump->far.x);

    fprintf(file, "Rayman Origins Recompiled Switch exception\n");
    fprintf(file, "error_desc=0x%08X esr=0x%08X\n", dump->error_desc, dump->esr);
    fprintf(file, "image_base=0x%016llX\n", static_cast<unsigned long long>(imageBase));
    fprintf(file, "pc=0x%016llX pc_offset=0x%016llX\n",
            static_cast<unsigned long long>(pc),
            static_cast<unsigned long long>(pc - imageBase));
    fprintf(file, "lr=0x%016llX lr_offset=0x%016llX\n",
            static_cast<unsigned long long>(dump->lr.x),
            static_cast<unsigned long long>(static_cast<uintptr_t>(dump->lr.x) - imageBase));
    fprintf(file, "sp=0x%016llX fp=0x%016llX far=0x%016llX\n",
            static_cast<unsigned long long>(dump->sp.x),
            static_cast<unsigned long long>(dump->fp.x),
            static_cast<unsigned long long>(far));

    if (g_memory.base != nullptr)
    {
        const uintptr_t guestBase = reinterpret_cast<uintptr_t>(g_memory.base);
        fprintf(file, "guest_base=0x%016llX\n", static_cast<unsigned long long>(guestBase));
        if (far >= guestBase && far - guestBase < PPC_MEMORY_SIZE)
            fprintf(file, "guest_fault=0x%08llX\n",
                    static_cast<unsigned long long>(far - guestBase));
    }

    if (PPCContext* context = GetPPCContext())
    {
        fprintf(file, "guest_lr=0x%08X guest_r1=0x%08X guest_r13=0x%08X\n",
                static_cast<uint32_t>(context->lr), context->r1.u32, context->r13.u32);
        fprintf(file, "guest_r3=0x%08X guest_r4=0x%08X guest_r5=0x%08X guest_r31=0x%08X\n",
                context->r3.u32, context->r4.u32, context->r5.u32, context->r31.u32);
    }

    for (unsigned i = 0; i < 29; ++i)
        fprintf(file, "x%u=0x%016llX\n", i,
                static_cast<unsigned long long>(dump->cpu_gprs[i].x));

    fclose(file);
}
}
