/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Sound output: the ring and its two back-ends (see frank_audio.h).
 */

#include "frank_audio.h"

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "board_config.h"
#include "hdmi.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"

/* I2S DMA block: 256 frames = 8 ms at 32 kHz */
#define I2S_FRAMES 256

static uint32_t ring[FRANK_AUDIO_RING]; /* L | R << 16 */
static volatile uint32_t ring_wr, ring_rd;
volatile uint32_t frank_audio_underruns;

static frank_audio_out_t cur_out = FRANK_AUDIO_OFF;
static bool hdmi_audio_up;
static bool i2s_up;
static i2s_config_t i2s_cfg;
static repeating_timer_t hdmi_timer;
static bool hdmi_timer_on;

const char *frank_audio_output_name(frank_audio_out_t out) {
    switch (out) {
    case FRANK_AUDIO_HDMI: return "HDMI";
    case FRANK_AUDIO_I2S:  return "I2S";
    default:               return "off";
    }
}

int frank_audio_level(void) {
    return (int)(ring_wr - ring_rd);
}

int frank_audio_free(void) {
    return FRANK_AUDIO_RING - frank_audio_level();
}

int frank_audio_push(const int16_t *lr, int frames) {
    uint32_t wr = ring_wr;
    int room = FRANK_AUDIO_RING - (int)(wr - ring_rd);
    if (frames > room) frames = room;
    for (int i = 0; i < frames; i++) {
        ring[wr & (FRANK_AUDIO_RING - 1)] = (uint16_t)lr[2 * i] | (uint32_t)(uint16_t)lr[2 * i + 1] << 16;
        wr++;
    }
    __dmb();
    ring_wr = wr;
    return frames;
}

/* one frame for a consumer; silence (the last value held) when empty */
static inline uint32_t __not_in_flash_func(ring_pop)(void) {
    static uint32_t last;
    uint32_t rd = ring_rd;
    if (rd == ring_wr) {
        frank_audio_underruns++;
        return last;
    }
    last = ring[rd & (FRANK_AUDIO_RING - 1)];
    ring_rd = rd + 1;
    return last;
}

/* ---- HDMI: 1 kHz timer, 32 frames per tick ---- */

static bool __not_in_flash_func(hdmi_tick)(repeating_timer_t *t) {
    (void)t;
    /* 32000 Hz / 1000 Hz: exactly 32 frames a tick */
    for (int i = 0; i < FRANK_AUDIO_RATE / 1000; i++) {
        uint32_t v = ring_pop();
        hdmi_audio_write_sample((int16_t)(v & 0xFFFF), (int16_t)(v >> 16));
    }
    return true;
}

/* ---- I2S: the DMA IRQ fills a ping-pong half ---- */

static void __not_in_flash_func(i2s_fill)(int idx, uint32_t *buf, uint32_t frames) {
    (void)idx;
    for (uint32_t i = 0; i < frames; i++) buf[i] = ring_pop();
}

static void stop_output(void) {
    if (hdmi_timer_on) {
        cancel_repeating_timer(&hdmi_timer);
        hdmi_timer_on = false;
    }
    if (i2s_up) {
        i2s_deinit(&i2s_cfg);
        i2s_up = false;
    }
}

void frank_audio_select(frank_audio_out_t out) {
    if (out == cur_out) return;
    stop_output();
    /* HDMI keeps its audio packets running (silence) whatever the output:
     * a sink that saw audio once may blank the picture if it goes away */
    if (!hdmi_audio_up) hdmi_audio_up = hdmi_audio_init();
    if (out == FRANK_AUDIO_HDMI) {
        if (!hdmi_audio_up) {
            printf("audio: HDMI audio unavailable (mode or heap)\n");
            out = FRANK_AUDIO_OFF;
        } else {
            hdmi_timer_on = add_repeating_timer_us(-1000, hdmi_tick, NULL, &hdmi_timer);
        }
    } else if (out == FRANK_AUDIO_I2S) {
        memset(&i2s_cfg, 0, sizeof(i2s_cfg));
        i2s_cfg.sample_freq = FRANK_AUDIO_RATE;
        i2s_cfg.channel_count = 2;
        i2s_cfg.data_pin = PICO_AUDIO_I2S_DATA_PIN;
        i2s_cfg.clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE;
        i2s_cfg.pio = pio0; /* pio2 is the video's */
        i2s_cfg.dma_trans_count = I2S_FRAMES;
        i2s_cfg.volume = 0;
        i2s_init(&i2s_cfg);
        i2s_set_fill_callback(i2s_fill);
        i2s_start();
        i2s_up = true;
    }
    cur_out = out;
    printf("audio: output %s\n", frank_audio_output_name(out));
}

void frank_audio_init(frank_audio_out_t out) {
    ring_wr = ring_rd = 0;
    frank_audio_select(out);
}

frank_audio_out_t frank_audio_output(void) {
    return cur_out;
}
