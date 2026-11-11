/* Freestanding test driver for the new, non-installed RV32 hook veneer. */
#include <stdint.h>

volatile uint32_t hook_test_intercept_args[3];
volatile uint32_t hook_test_intercept_calls;
volatile uint32_t hook_test_intercept_mode;
volatile uint32_t hook_test_callback_sp_mod_16;
volatile uint32_t hook_test_expected_sp;
volatile uint32_t hook_test_legacy_calls;
volatile uint32_t hook_test_legacy_entry_sp;
volatile uint32_t hook_test_legacy_observed[16];

extern int dp104_test_false_case(void);
extern int dp104_test_handled_case(void);
extern char dp104_test_false_after_hook;

static const uint32_t expected_t[7] = {
    0x70000010u, 0x70000011u, 0x70000012u, 0x70000013u,
    0x70000014u, 0x70000015u, 0x70000016u,
};

static const uint32_t expected_a[8] = {
    0x11223344u, 0x22334455u, 0x33445566u, 0x44556677u,
    0x55667788u, 0x66778899u, 0x778899aau, 0x1234abcdu,
};

int main(void)
{
    int result = dp104_test_false_case();
    if (result != 0) {
        return result;
    }
    if (hook_test_intercept_calls != 1u || hook_test_legacy_calls != 1u) {
        return 10;
    }
    if (hook_test_intercept_args[0] != expected_a[0] ||
        hook_test_intercept_args[1] != expected_a[1] ||
        hook_test_intercept_args[2] != expected_a[2]) {
        return 11;
    }
    if (hook_test_callback_sp_mod_16 != 0u ||
        hook_test_legacy_entry_sp != hook_test_expected_sp) {
        return 12;
    }
    if (hook_test_legacy_observed[0] !=
        (uint32_t)(uintptr_t)&dp104_test_false_after_hook) {
        return 13;
    }
    for (uint32_t index = 0; index < 7u; ++index) {
        if (hook_test_legacy_observed[index + 1u] != expected_t[index]) {
            return 14;
        }
    }
    for (uint32_t index = 0; index < 8u; ++index) {
        if (hook_test_legacy_observed[index + 8u] != expected_a[index]) {
            return 15;
        }
    }

#ifndef DP104_TASK_HOOK_TEST
    result = dp104_test_handled_case();
    if (result != 0) {
        return result;
    }
    if (hook_test_intercept_calls != 2u || hook_test_legacy_calls != 1u) {
        return 20;
    }
    if (hook_test_intercept_args[0] != expected_a[0] ||
        hook_test_intercept_args[1] != expected_a[1] ||
        hook_test_intercept_args[2] != expected_a[2]) {
        return 21;
    }
    if (hook_test_callback_sp_mod_16 != 0u) {
        return 22;
    }
#endif
    return 0;
}
