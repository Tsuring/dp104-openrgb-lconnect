#ifndef DP104_MASTER_TARGET_H
#define DP104_MASTER_TARGET_H

#include <stdint.h>

#define DP104_MASTER_RX_SNAPSHOT_OP 0xf6u
#define DP104_MASTER_RX_SNAPSHOT_SCHEMA 1u

#ifdef __cplusplus
extern "C" {
#endif

/* RawHID ABI: a0 is opaque, a1=port, a2=length. The target report is the
 * stock static buffer at 0x200104c0, not a0. Returns nonzero when D2 is
 * consumed; stock traffic and IN completion callbacks return zero. */
int dp104_master_host_intercept(uintptr_t opaque_a0, uintptr_t port,
                                unsigned length);

/* UART callback ABI. Before an explicit D2 session, input passes directly to
 * the original callback. During a session, a persistent stream parser handles
 * fragments/coalescing and legacy records are copied by the old callback. */
void dp104_master_uart_rx(const uint8_t *bytes, unsigned length);

/* Bounded UART RDA interrupt drain. It appends raw bytes to the stock receive
 * accumulator/buffer and does not invoke callbacks or touch LSR. */
void dp104_master_uart_rda_drain(void);

/* Called by the existing UART line-status IRQ branch after its single LSR
 * read. This records the sampled byte without reading the register again. */
void dp104_master_uart_line_status(uint8_t raw_lsr);

/* Regular task-context pump. Never call from an interrupt callback. */
void dp104_master_task_pump(void);

/* The linker maps this symbol to the original worker at RAM VMA 0x20001a56. */
void dp104_master_task_legacy(void);

#if defined(DP104_MASTER_TARGET_HOST_TEST)
extern volatile uint32_t dp104_master_test_csr800;
extern volatile uint8_t dp104_master_test_usb_active[5];
extern volatile uint32_t dp104_master_test_usb_remaining[5];
extern uint8_t dp104_master_test_report[32];
extern unsigned dp104_master_test_irq_enters;
extern unsigned dp104_master_test_irq_leaves;
extern int dp104_master_test_irq_depth;
extern int dp104_master_test_irq_fault;

uint8_t dp104_master_test_uart_status(void);
uint8_t dp104_master_test_uart_rfc(void);
uint16_t dp104_master_test_rx_accumulator(void);
uint8_t dp104_master_test_uart_rx_rfc(void);
uint8_t dp104_master_test_uart_rx_data(void);
volatile uint16_t *dp104_master_test_rx_accumulator_pointer(void);
volatile uint8_t *dp104_master_test_rx_buffer_pointer(void);
void dp104_master_test_uart_write(uint8_t byte);
void dp104_master_test_usb_in_writer(uintptr_t port, const uint8_t *bytes,
                                     unsigned length);
void dp104_master_test_usb_out_rearm(uintptr_t port, const uint8_t *bytes,
                                     unsigned length);
void dp104_master_test_before_usb_stage(void);
void dp104_master_test_legacy_uart_rx(const uint8_t *bytes, unsigned length);
#endif

#ifdef __cplusplus
}
#endif

#endif
