#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "audio.h"
#include "ivoice.h"
#include "intellivoice_minty.h"

/*
 * The FreeIntv/jzIntv-derived module exposes the instance as a global in
 * ivoice.c. It is not declared in ivoice.h, so declare it here in order to
 * consume only samples that have actually been generated.
 */
extern ivoice_t intellivoice;

#define IVOICE_WRITE_QUEUE_SIZE 32u
#define IVOICE_WRITE_QUEUE_MASK (IVOICE_WRITE_QUEUE_SIZE - 1u)

typedef struct
{
    uint8_t addr;      /* 0 = $0080 ALD, 1 = $0081 FIFO */
    uint16_t data;
} ivoice_write_t;

#if !IVOICE_DIRECT_BUS_WRITES
static volatile uint8_t ivoice_wr_head = 0;
static volatile uint8_t ivoice_wr_tail = 0;
static volatile uint32_t ivoice_wr_dropped = 0;
static volatile ivoice_write_t ivoice_wr_queue[IVOICE_WRITE_QUEUE_SIZE];
#endif

static uint32_t voice_cycle_step_fp = 0;
static uint32_t voice_cycle_frac = 0;
static int voice_read_pos = 0;

static void intellivoice_apply_pending_writes(void)
{
#if !IVOICE_DIRECT_BUS_WRITES
    while (ivoice_wr_tail != ivoice_wr_head)
    {
        uint8_t tail = ivoice_wr_tail;
        uint8_t addr = ivoice_wr_queue[tail].addr;
        uint16_t data = ivoice_wr_queue[tail].data;

        ivoice_wr_tail = (uint8_t)((tail + 1u) & IVOICE_WRITE_QUEUE_MASK);
        ivoice_wr(addr, data);
    }
#endif
}

void init_intellivoice(uint8_t tv_mode)
{
    uint32_t callback_rate;
    uint32_t cpu_rate;
    int pal_mode;

    voice_read_pos = 0;
    voice_cycle_frac = 0;

#if !IVOICE_DIRECT_BUS_WRITES
    ivoice_wr_head = 0;
    ivoice_wr_tail = 0;
    ivoice_wr_dropped = 0;
#endif

    pal_mode = (tv_mode == 0) ? 1 : 0;

    printf("Intellivoice init: %s mode\n", pal_mode ? "PAL" : "NTSC");

    ivoice_init(pal_mode, 1.0);
    ivoice_reset();

    /* Audio callback frequency is the audio callback frequency. */
    callback_rate = 1000000u / AUDIO_PERIOD;
    /* CPU rate is the master clock divided by 4, depending on PAL vs. NTSC. */
    cpu_rate = tv_mode? 894886u : 1000000u;

    /* Fixed-point cycles per audio callback, Q16.16. */
    voice_cycle_step_fp = (uint32_t)(((uint64_t)cpu_rate << 16) / callback_rate);
}

void intellivoice_reset(void)
{
    ivoice_reset();
    voice_read_pos = 0;
    voice_cycle_frac = 0;

#if !IVOICE_DIRECT_BUS_WRITES
    ivoice_wr_head = 0;
    ivoice_wr_tail = 0;
#endif
}

uint16_t intellivoice_read_bus(uint16_t addr)
{
    /*
     * Reads must return immediately to the CP1610 bus handler.
     * addr & 1 maps $0080->$0 and $0081->$1 for ivoice_rd().
     */
    return (uint16_t)ivoice_rd((uint32_t)(addr & 1u));
}

void intellivoice_write_bus(uint16_t addr, uint16_t data)
{
#if IVOICE_DIRECT_BUS_WRITES
    ivoice_wr((uint32_t)(addr & 1u), (uint32_t)data);
#else
    uint8_t head = ivoice_wr_head;
    uint8_t next = (uint8_t)((head + 1u) & IVOICE_WRITE_QUEUE_MASK);

    if (next == ivoice_wr_tail)
    {
        ivoice_wr_dropped++;
        return;
    }

    ivoice_wr_queue[head].addr = (uint8_t)(addr & 1u);
    ivoice_wr_queue[head].data = data;
    ivoice_wr_head = next;
#endif
}

int16_t intellivoice_next_sample(void)
{
    uint32_t cycles;
    int16_t sample = 0;

    intellivoice_apply_pending_writes();

    voice_cycle_frac += voice_cycle_step_fp;
    cycles = voice_cycle_frac >> 16;
    voice_cycle_frac &= 0xFFFFu;

    if (cycles != 0u)
        ivoice_tk(cycles);

    /*
     * Consume only newly generated samples.
     * ivoice.c increments intellivoice.cur_len when it appends to ivoiceBuffer.
     * If the low-level buffer wraps, cur_len is reset to 0, so reset our
     * consumer index as well.
     */
    if (voice_read_pos > intellivoice.cur_len)
        voice_read_pos = 0;

    if (voice_read_pos < intellivoice.cur_len)
        sample = ivoiceBuffer[voice_read_pos++];

    return sample;
}
