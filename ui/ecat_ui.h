/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the menus ask of the emulator, in C: the UI is frank-dino's, the
 * machine behind it is eCat3's, and this is all they say to each other.
 * Implemented in platform/ecat_main.cpp.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The settings this firmware keeps in /ecat/frank.ini. Every field is a
 * byte, because a menu row points at one. */
typedef struct {
    uint8_t audio_out;      /* 0 HDMI, 1 I2S */
    uint8_t volume;         /* 0..100 */
    uint8_t muted;
    uint8_t splash;         /* the boot screen with the machine list */
} ecat_settings_t;

extern ecat_settings_t ecat_settings;

void ecat_settings_defaults(void);
bool ecat_settings_save(void);
/* Applies every field, after a load or the defaults. */
void ecat_settings_apply(void);

/* Housekeeping while a menu waits: watchdog, USB keys (as menu_key bytes),
 * the console's ~...~ commands. */
void ecat_ui_poll(void);
int  ecat_ui_console_getchar(void);    /* -1: nothing */

bool ecat_ui_card_mounted(void);

/* The machines of this firmware */
const char *ecat_ui_family_name(void);
unsigned    ecat_ui_machine_count(void);
const char *ecat_ui_machine_title(unsigned i);
unsigned    ecat_ui_machine_current(void);
bool        ecat_ui_booting(void);      /* the power-on model list: nothing runs yet */
void        ecat_ui_start_machine(unsigned i);     /* reboots into it */
void        ecat_ui_reset(bool cold);
void        ecat_ui_bootsel(void);

/* Files: what one is for this machine (file_kind_t), and opening it */
int  ecat_ui_file_kind(const char *name);
const char *ecat_ui_software_dir(void);
bool ecat_ui_load(const char *path, int kind, char *detail, unsigned n);

/* The tape recorder: NULL when the machine has none */
const char *ecat_ui_tape_state(void);
bool        ecat_ui_tape_command(const char *command);

/* Live figures for the info page */
const char *ecat_ui_speed(void);
void        ecat_ui_info(char *buf, unsigned n);

#ifdef __cplusplus
}
#endif
