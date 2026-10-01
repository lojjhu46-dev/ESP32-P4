#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Audio path: I2S0 -> ES8311 DAC -> NS4150 amp -> speaker
 *             ES7210 mic ADC -> I2S0 -> P4
 * PA enable on GPIO11 is handled by the ES8311 driver (pa_pin).
 * Plays a short 1 kHz beep at startup to prove the chain works.
 */
esp_err_t app_audio_init(void);
void app_audio_beep(uint32_t freq_hz, uint32_t ms);

#ifdef __cplusplus
}
#endif
