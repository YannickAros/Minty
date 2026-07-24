#ifndef INTELLIVOICE_MINTY_H
#define INTELLIVOICE_MINTY_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Minty Intellivoice / SP0256 wrapper.
 *
 * Bus mapping, as used by the Intellivoice:
 *   $0080 : SP0256 ALD command write / LRQ status read
 *   $0081 : SPB-640 FIFO write / FIFO full status read
 *
 * The low-level SP0256 emulation is provided by ivoice.c / ivoice.h.
 */

#define IVOICE_ADDR_ALD   0x0080
#define IVOICE_ADDR_FIFO  0x0081


/*
 * If enabled, intellivoice_write_bus() calls ivoice_wr() directly from the
 * bus handler.
 *
 * Default is 0: bus writes are pushed into a small lock-free queue and then
 * applied from the audio callback context by intellivoice_next_sample().
 * This keeps Minty's timing-critical bus loop short and avoids running the
 * SP0256 state machine from core1 while audio generation may be active.
 */
#ifndef IVOICE_DIRECT_BUS_WRITES
#define IVOICE_DIRECT_BUS_WRITES 0
#endif

static inline bool intellivoice_is_addr(uint16_t addr)
{
    return (addr == IVOICE_ADDR_ALD) || (addr == IVOICE_ADDR_FIFO);
}

/* tv_mode follows existing Minty/ECS convention: 0 = PAL, non-zero = NTSC. */
void init_intellivoice(uint8_t tv_mode);
void intellivoice_reset(void);

/* Functions to be called from the Minty bus handler. */
uint16_t intellivoice_read_bus(uint16_t addr);
void intellivoice_write_bus(uint16_t addr, uint16_t data);

/*
 * Called once per ECS audio timer tick.
 * Advances SP0256 timing, applies pending bus writes, and returns the next
 */
int16_t intellivoice_next_sample(void);

#endif /* INTELLIVOICE_MINTY_H */
