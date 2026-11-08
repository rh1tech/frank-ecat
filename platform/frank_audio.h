/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sound output, one SRAM ring of 16-bit stereo frames at FRANK_AUDIO_RATE,
 * drained by the selected back-end, switchable at run time:
 *   HDMI: a 1 kHz timer on core 0 hands the frames to the HDMI driver
 *         (hdmi_audio_write_sample, up-sampled to 48 kHz data islands);
 *   I2S:  the DAC on GPIO 9-11 (pio0), its DMA IRQ fills the ping-pong
 *         buffers from the ring.
 * The producer (the emulator, core 0) pushes whole blocks and watches the
 * ring level to pace itself.
 */

#ifndef FRANK_AUDIO_H
#define FRANK_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRANK_AUDIO_RATE  32000
#define FRANK_AUDIO_RING  4096 /* stereo frames, power of two (128 ms) */

typedef enum {
    FRANK_AUDIO_OFF = 0,
    FRANK_AUDIO_HDMI,
    FRANK_AUDIO_I2S,
} frank_audio_out_t;

/* after the HDMI output is up (its audio needs the video mode) */
void frank_audio_init(frank_audio_out_t out);
void frank_audio_select(frank_audio_out_t out);
frank_audio_out_t frank_audio_output(void);
const char *frank_audio_output_name(frank_audio_out_t out);

/* frames: interleaved L,R. Returns the frames taken (the rest did not fit). */
int frank_audio_push(const int16_t *lr, int frames);
/* frames waiting in the ring / free in it */
int frank_audio_level(void);
int frank_audio_free(void);

/* counters since boot */
extern volatile uint32_t frank_audio_underruns;

#ifdef __cplusplus
}
#endif

#endif /* FRANK_AUDIO_H */
