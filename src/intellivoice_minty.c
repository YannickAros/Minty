#include <stdint.h>
#include <stdbool.h>

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

static uint8_t voice_volume = IVOICE_DEFAULT_VOLUME;
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

static uint32_t intellivoice_cpu_rate_for_mode(uint8_t tv_mode)
{
    /*
     * ivoice_tk() uses the same CPU-side time base as FreeIntv:
     * NTSC master clock 3579545 Hz / 4 = 894886.25 Hz.
     * PAL  master clock 4000000 Hz / 4 = 1000000 Hz.
     *
     * tv_mode convention is copied from ecs.c:
     *   0        => PAL
     *   non-zero => NTSC
     */
    if (tv_mode == 0)
        return 1000000u;

    return 894886u;
}

void init_intellivoice(uint8_t tv_mode, uint8_t volume)
{
    uint32_t callback_rate;
    uint32_t cpu_rate;
    int pal_mode;

    voice_volume = volume;
    voice_read_pos = 0;
    voice_cycle_frac = 0;

#if !IVOICE_DIRECT_BUS_WRITES
    ivoice_wr_head = 0;
    ivoice_wr_tail = 0;
    ivoice_wr_dropped = 0;
#endif

    pal_mode = (tv_mode == 0) ? 1 : 0;
    ivoice_init(pal_mode, 1.0);
    ivoice_reset();

    /* Audio callback frequency is the ECS timer frequency. */
    callback_rate = 1000000u / AUDIO_PERIOD;
    cpu_rate = intellivoice_cpu_rate_for_mode(tv_mode);

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

    /* 8-bit volume scale, same style as ECS volume usage. */
    sample = (int16_t)(((int32_t)sample * (int32_t)(voice_volume + 1u)) >> 8);

    return sample;
}

int16_t intellivoice_next_pwm_delta(void)
{
    /*
     * Convert signed 16-bit-ish voice output into a signed contribution for
     * Minty's 10-bit PWM domain. This value should be added around midpoint
     * and clipped to 0..PWM_WRAP by the caller.
     */
    return (int16_t)((int32_t)intellivoice_next_sample() >> 7);
}

uint32_t intellivoice_dropped_writes(void)
{
#if IVOICE_DIRECT_BUS_WRITES
    return 0;
#else
    return ivoice_wr_dropped;
#endif
}

uint8_t intellivoice_pending_writes(void)
{
#if IVOICE_DIRECT_BUS_WRITES
    return 0;
#else
    return (uint8_t)((ivoice_wr_head - ivoice_wr_tail) & IVOICE_WRITE_QUEUE_MASK);
#endif
}
