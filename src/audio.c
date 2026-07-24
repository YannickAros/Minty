#if CONFIG_ECS_AUDIO || CONFIG_INTELLIVOICE

#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/pwm.h"

#include "board.h"
#include "audio.h"
#include "emu2149.h"
#include "intellivoice_minty.h"

static repeating_timer_t timer;
PSG* psg0;
uint8_t AudioVolume;

const uint8_t ECS_LUT[16] = {
   0x00,
   0x02,
   0x04,
   0x0B,
   0x01,
   0x03,
   0x05,
   0x0C,
   0x07,
   0x06,
   0x0D,
   0x08,
   0x09,
   0x0A,
   0x0E,
   0x0F
};

bool audio_callback(repeating_timer_t *rt) {   
   int32_t ecs_raw = 0;
   int32_t ivoice_raw = 0;
   uint16_t pwm = 0;

#if CONFIG_ECS_AUDIO
   PSG_calc(psg0);

   /* Mix 3 ECS PSG channels, apply 8-bit volume control, normalize to 10-bit PWM. */
   ecs_raw = (int32_t)psg0->ch_out[0] +
                     (int32_t)psg0->ch_out[1] +
                     (int32_t)psg0->ch_out[2];
#endif

#if CONFIG_INTELLIVOICE
   /*
    * Intellivoice output is signed, mixig TBD.  For now, just use the raw output from the Intellivoice PWM delta generator.
    * The Intellivoice PWM delta generator is already normalized to 10-bit PWM,
    */
    ivoice_raw = (int32_t)intellivoice_next_pwm_delta();
#endif

   pwm = (abs(ecs_raw+ivoice_raw) * (int32_t)(AudioVolume + 1) / 3) >> 10;
   pwm_set_gpio_level(AUDIO_PIN, pwm);
   return true;
}
void init_audio(uint8_t tv_mode, uint8_t volume) {

   gpio_set_function(AUDIO_PIN, GPIO_FUNC_PWM);
   uint audioSlice = pwm_gpio_to_slice_num(AUDIO_PIN);
   AudioVolume = volume;

   pwm_config cfg = pwm_get_default_config();
   pwm_config_set_clkdiv(&cfg, 1.0f);
   pwm_config_set_wrap(&cfg, PWM_WRAP);
   pwm_init(audioSlice, &cfg, true);

#if CONFIG_ECS_AUDIO
   if (tv_mode == 0) 
      psg0 = PSG_new(PAL_ECS_FREQ/2, 1000000/AUDIO_PERIOD);
   else
      psg0 = PSG_new(NTSC_ECS_FREQ/2, 1000000/AUDIO_PERIOD);

   PSG_reset(psg0);
   PSG_setVolumeMode(psg0, EMU2149_VOL_AY_3_8910);
#endif
#if CONFIG_INTELLIVOICE
   init_intellivoice(tv_mode, AudioVolume);
#endif

   add_repeating_timer_us(-AUDIO_PERIOD, audio_callback, NULL, &timer);

}

#endif
