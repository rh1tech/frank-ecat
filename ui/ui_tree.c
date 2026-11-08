/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2), the menus from frank-dino
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The settings tree: frank-dino's ui_tree.c, over this firmware. A row
 * points straight at its byte in ecat_settings and runs its hook when it
 * changes; nothing is staged, and "Save settings to the card" is what
 * makes a change outlive the power.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ecat_ui.h"
#include "ui_dialog.h"
#include "ui_model.h"
#include "ui_nav.h"
#include "ui_render.h"

/* ── values ──────────────────────────────────────────────────────────── */

int32_t ui_node_value(const ui_node_t *n) {
    return n->slot ? (int32_t)*n->slot : 0;
}

void ui_node_set_value(const ui_node_t *n, const int32_t v) {
    if (!n->slot) return;
    if ((int32_t)*n->slot == v) return;
    *n->slot = (uint8_t)v;
    if (n->changed) n->changed();
}

const char *ui_node_value_label(const ui_node_t *n) {
    static char buf[UI_DYN_VALUE_LEN];

    if (n->vlabel) return n->vlabel();

    switch (n->kind) {
        case K_RADIO:
        case K_BOOL: {
            const int32_t v = ui_node_value(n);
            uint8_t cnt;
            const ui_option_t *o = ui_node_options(n, &cnt);
            for (uint8_t i = 0; i < cnt; i++)
                if (o[i].value == v) return o[i].slabel ? o[i].slabel : o[i].label;
            return "";
        }
        case K_INT:
            snprintf(buf, sizeof buf, "%ld", (long)ui_node_value(n));
            return buf;
        default:
            return "";
    }
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ui_status(buf);
}

/* ── machine ─────────────────────────────────────────────────────────── */

/* The models of this firmware, one row each, the running one marked. */
static void models_build(ui_dynrows_t *d) {
    const unsigned cur = ecat_ui_machine_current();
    for (unsigned i = 0; i < ecat_ui_machine_count(); i++)
        ui_dyn_add(d, ecat_ui_machine_title(i), i == cur ? (ecat_ui_booting() ? "last used" : "running") : "", (int32_t)i, false);
    ui_dyn_focus_tag(d, (int32_t)cur);
}

static void models_key(const int32_t tag, const uint8_t key) {
    if (key) return;
    if (ecat_ui_booting()) { ecat_ui_start_machine((unsigned)tag); return; }
    if ((unsigned)tag == ecat_ui_machine_current()) {
        say("%s is running already.", ecat_ui_machine_title((unsigned)tag));
        return;
    }
    char q[96];
    snprintf(q, sizeof q, "Start %s?\nAnything in memory will be lost.",
             ecat_ui_machine_title((unsigned)tag));
    if (!ui_confirm(q, "Change the machine", true)) { say("Left alone."); return; }
    ecat_ui_start_machine((unsigned)tag);           /* does not come back */
}

static const char *model_info(void) {
    static char buf[UI_DYN_VALUE_LEN];
    snprintf(buf, sizeof buf, "%u to choose", ecat_ui_machine_count());
    return buf;
}

static void reset_action(void) {
    if (!ui_confirm("Reset the machine?\nThe program in memory stays.", "Reset", true)) {
        say("Left running.");
        return;
    }
    ecat_ui_reset(false);
    say("Reset.");
    ui_request_close();
}

static void cold_action(void) {
    if (!ui_confirm("Power the machine off and on?\nAnything in memory will be lost.",
                    "Cold restart", false)) {
        say("Left running.");
        return;
    }
    ecat_ui_reset(true);
    say("Restarted.");
    ui_request_close();
}

static bool running(void) { return !ecat_ui_booting(); }

static const char *speed_info(void) { return ecat_ui_speed(); }

static const ui_node_t machine_kids[] = {
    UI_DYN    ("Model",        models_build, models_key, model_info),
    UI_ACTIONV_EN("Reset",        reset_action, NULL, running),
    UI_ACTIONV_EN("Cold restart", cold_action, NULL, running),
    UI_ACTIONV_EN("Speed",       NULL, speed_info, running),
    UI_BOOL   ("List at power-on", &ecat_settings.splash, NULL),
};

/* ── tape ────────────────────────────────────────────────────────────── */

static bool has_tape(void) { return ecat_ui_tape_state() != NULL; }

static const char *tape_info(void) {
    const char *s = ecat_ui_tape_state();
    return s ? s : "none";
}

static void tape_do(const char *cmd, const char *done) {
    if (ecat_ui_tape_command(cmd)) say("%s", done);
    else                           say("The tape recorder said no.");
}

static void tape_play(void)   { tape_do("play",   "Playing. Start the machine's load command."); }
static void tape_stop(void)   { tape_do("stop",   "Stopped."); }
static void tape_rewind(void) { tape_do("rewind", "Rewound to the start."); }

static const ui_node_t tape_kids[] = {
    UI_ACTIONV("Tape",   NULL, tape_info),
    UI_ACTION ("Play",   tape_play),
    UI_ACTION ("Stop",   tape_stop),
    UI_ACTION ("Rewind", tape_rewind),
};

/* ── audio ───────────────────────────────────────────────────────────── */

static const ui_option_t opt_output[] = {
    { "HDMI",      0, NULL },
    { "I2S board", 1, "I2S" },
};

static void audio_changed(void) {
    ecat_settings_apply();
    say(ecat_settings.audio_out ? "Sound on the I2S board." : "Sound over HDMI.");
}

static void level_changed(void) { ecat_settings_apply(); }

static const ui_node_t audio_kids[] = {
    UI_RADIO("Output", &ecat_settings.audio_out, opt_output, audio_changed),
    UI_INT  ("Volume", &ecat_settings.volume, 0, 100, 5, level_changed),
    UI_BOOL ("Mute",   &ecat_settings.muted, level_changed),
};

/* ── system ──────────────────────────────────────────────────────────── */

static char page_buf[2048];

static void info_page(void) {
    ecat_ui_info(page_buf, sizeof page_buf);
    ui_text_page("Emulator info", page_buf, ecat_ui_machine_title(ecat_ui_machine_current()));
}

static void save_action(void) {
    if (!ecat_ui_card_mounted()) { say("No card to save to."); return; }
    if (ecat_settings_save()) { say("Saved to /ecat/frank.ini"); return; }
    ui_toast("Could not write /ecat/frank.ini", true, 1500);
    say("Not saved.");
}

static void defaults_action(void) {
    if (!ui_confirm("Put every setting back\nas it came?", "Restore defaults", false)) {
        say("Left alone.");
        return;
    }
    ecat_settings_defaults();
    ecat_settings_apply();
    ui_mark_dirty(D_ALL);
    say("Defaults restored.");
}

static void bootsel_action(void) {
    if (!ui_confirm("Restart into the USB boot loader,\nto take a new firmware?",
                    "Firmware update", false)) {
        say("Left alone.");
        return;
    }
    ecat_ui_bootsel();
}

static const ui_node_t system_kids[] = {
    UI_ACTION("Firmware update (BOOTSEL)", bootsel_action),
};

static const ui_node_t root[] = {
    UI_SUB   ("Machine", machine_kids, NULL),
    UI_SUB   ("Tape",    tape_kids,    has_tape),
    UI_SUB   ("Audio",   audio_kids,   NULL),
    UI_SUB   ("System",  system_kids,  NULL),
    UI_PAGE  ("Emulator info", info_page),
    UI_ACTION("Save settings to the card", save_action),
    UI_ACTION("Restore defaults",          defaults_action),
};

const ui_node_t *ui_root_nodes(void) { return root; }
uint8_t          ui_root_count(void) { return UI_COUNT(root); }

const char *ui_machine_line(void) {
    return ecat_ui_machine_title(ecat_ui_machine_current());
}
