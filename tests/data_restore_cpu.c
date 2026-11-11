#include <stddef.h>
#include <stdint.h>

#ifndef DP104_INITDATA_LENGTH
#error DP104_INITDATA_LENGTH must be supplied by the test build
#endif

extern int dp104_restore_data(void);
extern const uint8_t __dp104_original_reference[];
extern uint8_t __dp104_initdata_ram[];
extern uint8_t __dp104_dst_guard_before[];
extern uint8_t __dp104_dst_guard_after[];

static int run_test(void)
{
    size_t i;

    if (sizeof(void *) != 4u)
        return 1;

    __dp104_dst_guard_before[0] = 0xa5;
    __dp104_dst_guard_after[0] = 0x5a;
    for (i = 0; i < (size_t)DP104_INITDATA_LENGTH; ++i)
        __dp104_initdata_ram[i] = 0xc3;

    if (dp104_restore_data() != 1)
        return 2;
    if (__dp104_dst_guard_before[0] != 0xa5 ||
        __dp104_dst_guard_after[0] != 0x5a)
        return 3;

    for (i = 0; i < (size_t)DP104_INITDATA_LENGTH; ++i)
    {
        if (__dp104_initdata_ram[i] != __dp104_original_reference[i])
            return 4;
    }
    return 0;
}

static __attribute__((noreturn)) void linux_exit(int status)
{
#if defined(__riscv)
    register long argument __asm__("a0") = status;
    register long syscall_number __asm__("a7") = 93;
    __asm__ volatile("ecall" : : "r"(argument), "r"(syscall_number) : "memory");
#elif defined(__arm__)
    register long argument __asm__("r0") = status;
    register long syscall_number __asm__("r7") = 1;
    __asm__ volatile("svc #0" : : "r"(argument), "r"(syscall_number) : "memory");
#else
#error This harness supports only RV32 and ARM32.
#endif
    __builtin_unreachable();
}

__attribute__((noreturn, used)) void _start(void)
{
    linux_exit(run_test());
}
