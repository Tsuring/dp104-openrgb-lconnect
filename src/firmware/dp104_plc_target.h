#ifndef DP104_PLC_TARGET_H
#define DP104_PLC_TARGET_H

#include <stdint.h>
#include "dp104_patch_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DP104_PLC_TARGET_MAGIC UINT32_C(0x504c4331)
#define DP104_PLC_CAL_RAW_PULSE 0xf0u
#define DP104_PLC_CAL_SET_ORDER 0xf1u
#define DP104_PLC_DIAGNOSTIC_MS 1000u
#define DP104_PLC_QUEUE_DATA_BYTES 67u
#define DP104_PLC_QUEUE_SLOT_BYTES 68u

/* The target linker owns both definitions; this API only initializes/uses them. */
extern dp104_plc_patch_state __dp104_plc_state;
extern const uint32_t __dp104_twiddle_basis[DP104_TWIDDLE_WORDS];

/* Called once after BSS initialization. Magic is written only after all setup. */
int dp104_plc_target_init(void);

/* Normal task-context entry; polls timers even when the UART queue is empty. */
void dp104_plc_target_poll(void);

/* UART RX callback: bounded copy only; protocol work runs in task context. */
void dp104_plc_uart_rx(const uint8_t *data, uint32_t length);
/* Circular RX drains from half/full DMA interrupts and USART IDLE. */
void dp104_plc_dma_irq(void);
void dp104_plc_usart_irq(void);

/* Replacement for the stock queue-pop wrapper. Any returned legacy slot is stable. */
void dp104_plc_queue_hook(uint8_t **out);

/* Thumb setter veneer reads only this volatile ownership flag. */
int dp104_plc_should_skip_setter(void);

#ifdef DP104_PLC_HOST_TEST
/* Host-only fakes. No target addresses, MMIO, IRQ instructions, or vendor calls. */
uint32_t dp104_plc_test_millis(void);
uint32_t dp104_plc_test_irq_save(void);
void dp104_plc_test_irq_restore(uint32_t primask);
void dp104_plc_test_legacy_pop(uint8_t **out);
void dp104_plc_test_uart_send(uint8_t *bytes, uint32_t length, uint32_t mode);
uint32_t dp104_plc_test_dma_flags(void);
uint32_t dp104_plc_test_dma_remaining(void);
void dp104_plc_test_dma_clear(uint32_t flags);
void dp104_plc_test_dma_enable_irq(void);
uint32_t dp104_plc_test_uart_status(void);
uint32_t dp104_plc_test_uart_data(void);
uint32_t dp104_plc_test_uart_control(void);
extern volatile uint8_t dp104_plc_test_key_buffer[DP104_FRAME_BYTES];
extern volatile uint8_t dp104_plc_test_dirty;
#endif

#ifdef __cplusplus
}
#endif
#endif
