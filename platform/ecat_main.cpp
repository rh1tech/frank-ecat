/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The emulator firmware: one family of machines per UF2 (the flash VFS holds
 * their configurations and ROMs, tools/embed_vfs.py), eCat3's core driven
 * without threads (Emulator::run(), slice(), render_tick(), ECAT_SINGLE_THREAD):
 *   core 0: the emulation, sound, input;
 *   core 1: the HDMI line IRQ.
 * The display draws palette indices straight into the scanned-out surface
 * (FrankRenderer); the sound is pulled from the core's mixer into the HDMI/I2S
 * ring (FrankAudioDriver).
 *
 * Hangs: a 5 s watchdog fed by the main loop, breadcrumbs (platform_crumb)
 * at every stage; after a hang the next boot reports where and enters BOOTSEL.
 *
 * Keys (USB keyboard): F10 sound HDMI <-> I2S, F11 the machine menu, F12 reset.
 *
 * Console: a typed line goes to the machine (Emulator::paste_text, Enter ends
 * it); "~C m N~" reboots into machine N of the family, "~C s~" prints the
 * status, "~BOOT~" / "~RST~" as in the diag firmware.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#include "emulator/emulator.h"
#include "emulator/audio/audio_driver.h"
#include "emulator/indexed_palette.h"
#include "emulator/files.h"

extern "C" {
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "platform.h"
#include "frank_audio.h"
#include "frank_vfs.h"
#include "graphics.h"
#include "hdmi.h"
#include "ff.h"
#if USB_HID_ENABLED
#include "usbhid.h"
#endif
}
#include "frank_keys.h"
#include "emulator/devices/common/tape.h"
#include "emulator/devices/common/fdd.h"
extern "C" {
#include "vga.h"
#include "ecat_ui.h"
#include "storage.h"
#include "menu.h"
#include "ui_dialog.h"
#include "ui_model.h"
#include "ui_nav.h"
}

#ifndef ECAT_VIDEO_MODE
#define ECAT_VIDEO_MODE 28          /* 1024x768 @50 (graphics/video_mode_table.h) */
#endif
#ifndef ECAT_REFRESH
#define ECAT_REFRESH 5000           /* Hz x 100 */
#endif
#ifndef ECAT_SURFACE_W
#define ECAT_SURFACE_W 640
#endif
#ifndef ECAT_SURFACE_H
#define ECAT_SURFACE_H 300
#endif

/* ---- the surface: palette indices, one byte a dot, scanned out as it is ---- */

static uint8_t surface[ECAT_SURFACE_W * ECAT_SURFACE_H];

static volatile int surface_w = 468, surface_h = 300;

extern "C" uint8_t *__not_in_flash_func(getLineBuffer)(int line) {
    return surface + line * surface_w;
}


extern "C" int __not_in_flash_func(get_video_mode)(void) {
    return ECAT_VIDEO_MODE;
}

static volatile uint32_t vsyncs;
extern "C" void __not_in_flash_func(ESPectrum_vsync)(void) {
    vsyncs++;
}

/* the largest whole scale that fits the mode's active area: x1..x4 across,
 * x2..x4 down (the line map needs runs of at least two lines) */
static void layout_for(int w, int h, int *hs, int *vs) {
    const video_mode_t m = graphics_get_video_mode(ECAT_VIDEO_MODE);
    const int act_w = m.screen_width * 2, act_h = m.v_active;
    int x = act_w / (w > 0 ? w : 1), y = act_h / (h > 0 ? h : 1);
    if (x < 1) x = 1;
    if (x > 4) x = 4;
    if (y < 2) y = 2;
    if (y > 4) y = 4;
    *hs = x;
    *vs = y;
}

class FrankRenderer : public VideoRenderer
{
public:
    std::string get_name() override { return "FRANK HDMI"; }

    void init_screen(void *p, int sx, int sy, double ss, double ps) override
    {
        VideoRenderer::init_screen(p, sx, sy, ss, ps);
        apply(sx, sy);
    }
    void stop() override {}
    uint8_t *get_buffer() override { return surface; }
    int get_line_bytes() override { return screen_x; }
    void fill(uint32_t c) override { memset(surface, (int)(c & 0xFF), sizeof(surface)); }
    void resize(int sx, int sy, double ss, double ps) override
    {
        screen_ss = ss;
        screen_ps = ps;
        apply(sx, sy);
    }
    void render() override {}       // the surface is the picture
    uint32_t MapRGB(uint8_t R, uint8_t G, uint8_t B) override
    {
        const uint32_t rgb = (uint32_t)R << 16 | (uint32_t)G << 8 | B;
        bool added = false;
        const uint8_t i = m_palette.map(rgb, &added);
        if (added) graphics_set_palette(i, rgb);
        return i;
    }
    std::vector<uint8_t> get_screenshot() override
    {
        std::vector<uint8_t> image((size_t)screen_x * screen_y * 4);
        for (size_t i = 0; i < (size_t)screen_x * screen_y; i++) {
            const uint32_t c = m_palette.rgb(surface[i]);
            image[i * 4 + 0] = (uint8_t)(c >> 16);
            image[i * 4 + 1] = (uint8_t)(c >> 8);
            image[i * 4 + 2] = (uint8_t)c;
            image[i * 4 + 3] = 0xFF;
        }
        return image;
    }

    void install_black() { graphics_set_palette(0, 0); }
    unsigned palette_count() const { return m_palette.count(); }

private:
    IndexedPalette m_palette;

    void apply(int sx, int sy)
    {
        if (sx > ECAT_SURFACE_W) sx = ECAT_SURFACE_W;
        if (sy > ECAT_SURFACE_H) sy = ECAT_SURFACE_H;
        if (sx < 4) sx = 4;
        if (sy < 1) sy = 1;
        sx &= ~3;
        screen_x = sx;
        screen_y = sy;
        surface_w = sx;
        surface_h = sy;
        memset(surface, 0, sizeof(surface));
        int hs, vs;
        layout_for(sx, sy, &hs, &vs);
        hdmi_set_layout(sx, sy, hs, vs);
        printf("video: %dx%d x%d/x%d\n", sx, sy, hs, vs);
    }
};

/* ---- sound: the core's mixers pulled into the output ring ----
 *
 * Every sound device of the machine opens a driver of its own, as each opens
 * its own stream on the desktop (and the core deletes the driver it closes):
 * the pump mixes all of them, saturated, into the HDMI/I2S ring. */

class FrankAudioDriver;
static std::vector<FrankAudioDriver *> audio_drivers;

class FrankAudioDriver : public AudioDriver
{
public:
    FrankAudioDriver() { audio_drivers.push_back(this); }
    ~FrankAudioDriver() override
    {
        for (size_t i = 0; i < audio_drivers.size(); i++)
            if (audio_drivers[i] == this) { audio_drivers.erase(audio_drivers.begin() + i); break; }
    }
    bool open(int sampleRate, int channels, int bufferSamples, AudioCallback callback, void *userdata) override
    {
        (void)sampleRate;
        (void)bufferSamples;
        m_channels = channels;
        m_callback = callback;
        m_userdata = userdata;
        return true;
    }
    int getObtainedSampleRate() const override { return FRANK_AUDIO_RATE; }
    int getObtainedBufferSamples() const override { return CHUNK; }
    void start() override { m_running = true; }
    void stop() override { m_running = false; }
    void close() override { m_callback = nullptr; }

    enum { CHUNK = 256 };

    /* adds CHUNK stereo frames of this device to mix[] */
    void pull(int32_t *mix)
    {
        if (!m_running || !m_callback) return;
        int16_t in[CHUNK * 2];
        m_callback(m_userdata, reinterpret_cast<uint8_t *>(in), CHUNK * m_channels * 2);
        if (m_channels == 1)
            for (int i = 0; i < CHUNK; i++) { mix[2 * i] += in[i]; mix[2 * i + 1] += in[i]; }
        else
            for (int i = 0; i < CHUNK * 2; i++) mix[i] += in[i];
    }

    int channels() const { return m_channels; }
    void *owner() const { return m_userdata; }
    bool live() const { return m_running && m_callback; }

private:
    int m_channels = 1;
    AudioCallback m_callback = nullptr;
    void *m_userdata = nullptr;
    bool m_running = false;
};

static uint32_t audio_chunks;
static int audio_peak;

/* main loop: as many chunks as the ring has room for */
static void audio_pump(void)
{
    if (audio_drivers.empty()) return;
    while (frank_audio_free() >= FrankAudioDriver::CHUNK) {
        int32_t mix[FrankAudioDriver::CHUNK * 2] = {0};
        for (FrankAudioDriver *d : audio_drivers) d->pull(mix);
        int16_t out[FrankAudioDriver::CHUNK * 2];
        for (int i = 0; i < FrankAudioDriver::CHUNK * 2; i++) {
            int32_t v = mix[i];
            if (v > 32767) v = 32767;
            else if (v < -32768) v = -32768;
            out[i] = (int16_t)v;
            const int a = v < 0 ? -v : v;
            if (a > audio_peak) audio_peak = a;
        }
        frank_audio_push(out, FrankAudioDriver::CHUNK);
        audio_chunks++;
    }
}

AudioDriver *ecat_create_audio_driver()
{
    return new FrankAudioDriver();
}

/* Every C++ throw, while ecat_trace_throws is set: the type and where from
 * (a hang in the unwinder says nothing about which exception it was) */
#include <typeinfo>
static volatile bool ecat_trace_throws;
extern "C" void __real___cxa_throw(void *, std::type_info *, void (*)(void *));
extern "C" void __wrap___cxa_throw(void *obj, std::type_info *t, void (*dest)(void *))
{
    if (ecat_trace_throws) {
        printf("throw %s from %p\n", t ? t->name() : "?", __builtin_return_address(0));
        stdio_flush();
    }
    __real___cxa_throw(obj, t, dest);
}

/* the desktop's cache of unpacked extension files (emulator/cache.cpp, not
 * built here: the firmware's extensions are flattened at build time) */
void cache_touch(const std::string &) {}

/* ---- core 1: the HDMI line IRQ ---- */

/* The emulator whose second-core processors this core runs (set_parallel()) */
static Emulator * volatile par_emu;

static unsigned int core_num(void) { return get_core_num(); }

/* Moving the HDMI line IRQ to core 0 (hdmi_irq_to_core0()): core 1 lets go
 * of it first, so the two never take one line each */
extern "C" int hdmi_video_irq(void);
extern "C" void hdmi_restart(void);
extern "C" void hdmi_report_state(void);
extern "C" void hdmi_report_late(void);
static volatile int hdmi_irq_release;   /* 1: core 1 to disable it, 2: done */
/* A display of the second core is drawn there, between its quanta (render_tick):
 * core 0 then has only the processors it runs. A menu takes the screen
 * after core 1 has let go of it (core1_render_hold, core1_rendering) */
static volatile bool core1_renders, core1_render_hold, core1_rendering;
static volatile int hdmi_irq_core = 1;   /* the core that takes it */
static volatile bool hdmi_restart_req;   /* core 1: restart the scan-out */
static uint32_t hdmi_restarts;

static void hdmi_irq_to_core0(void)
{
    hdmi_irq_release = 1;
    while (hdmi_irq_release != 2) tight_loop_contents();
    irq_set_priority(hdmi_video_irq(), 0);
    irq_set_enabled(hdmi_video_irq(), true);
    hdmi_irq_core = 0;
    printf("hdmi: line IRQ on core 0\n");
}

static void core1_main(void)
{
    graphics_init();
    graphics_set_bgcolor(0x000000);
    platform_prof_core1_init();
    multicore_fifo_push_blocking(1);
    /* The HDMI line IRQ, and between its lines the quanta of the machine's
     * second-core processors, as the first core hands them out */
    while (1) {
        if (hdmi_irq_release == 1) {
            irq_set_enabled(hdmi_video_irq(), false);
            hdmi_irq_release = 2;
        }
        if (hdmi_restart_req) {
            hdmi_restart();
            hdmi_restart_req = false;
        }
        Emulator * e = par_emu;
        if (e) {
            e->parallel_core1_step();
            if (core1_renders && !core1_render_hold) {
                core1_rendering = true;
                __dmb();
                if (!core1_render_hold) e->render_tick();
                core1_rendering = false;
            }
        }
    }
}

/* the machine to boot, kept over a watchdog reboot */
static uint32_t __attribute__((section(".uninitialized_data"))) boot_machine[2];
#define BOOT_MAGIC 0xEC47B007u
#define MENU_BOOT_MAGIC 0xEC47AE4Bu     /* boot to the model list */

static FrankRenderer *renderer;
static Emulator *emu;

extern "C" uint32_t frank_audio_tick_us, frank_audio_ticks;
extern "C" volatile uint32_t hdmi_irq_max_dur_us, hdmi_irq_max_gap_us, hdmi_irq_dur_total_us;
extern "C" volatile uint32_t irq_inx;

static void status(void)
{
    ecat_heap_report();
    unsigned live = 0;
    for (FrankAudioDriver *d : audio_drivers) live += d->live();
    printf("audio: %u drivers (%u live), %lu chunks, peak %d\n", (unsigned)audio_drivers.size(), live,
           (unsigned long)audio_chunks, audio_peak);
    audio_peak = 0;
    printf("vsync %lu, audio ring %d, underruns %lu\n", (unsigned long)vsyncs, frank_audio_level(),
           (unsigned long)frank_audio_underruns);
    printf("hdmi audio tick: %lu us in %lu ticks\n", (unsigned long)frank_audio_tick_us, (unsigned long)frank_audio_ticks);
    printf("core 1 stack: %u bytes used of %u\n", platform_core1_stack_used(), 16u * 1024u);
    printf("hdmi line irq: longest %lu us, largest gap %lu us, %lu irqs, core %d\n", (unsigned long)hdmi_irq_max_dur_us,
           (unsigned long)hdmi_irq_max_gap_us, (unsigned long)irq_inx, hdmi_irq_core);
    hdmi_irq_max_dur_us = hdmi_irq_max_gap_us = 0;
    {
        static uint32_t last_total;
        static uint64_t last_t;
        const uint64_t now = time_us_64();
        const uint32_t tot = hdmi_irq_dur_total_us;
        if (last_t) printf("hdmi line irq: %lu%% of core 1\n", (unsigned long)((uint64_t)(tot - last_total) * 100 / (now - last_t)));
        last_total = tot;
        last_t = now;
    }
    frank_audio_tick_us = frank_audio_ticks = 0;
}

/* "~C put PATH SIZE~", then SIZE raw bytes: a file to the SD card, made
 * with its directories (tools/upload.py) - software onto the card with nobody
 * at the board */
static void console_put(const char *path, unsigned size)
{
    char dir[200];
    strncpy(dir, path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') { *p = 0; f_mkdir(dir); *p = '/'; }
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("PUT FAIL open %s\n", path);
        return;
    }
    printf("PUT READY %u\n", size);
    static uint8_t buf[512];
    unsigned got = 0, n = 0;
    bool ok = true;
    while (got < size) {
        platform_watchdog_feed();
        int c = getchar_timeout_us(3000000);
        if (c < 0) { ok = false; break; }
        buf[n++] = (uint8_t)c;
        got++;
        if (n == sizeof(buf) || got == size) {
            UINT w;
            if (f_write(&f, buf, n, &w) != FR_OK || w != n) { ok = false; break; }
            n = 0;
        }
    }
    f_close(&f);
    printf(ok ? "PUT OK %u\n" : "PUT FAIL at %u\n", got);
}

/* Typed lines wait here, and go to the machine one at a time: a new
 * paste_text() replaces one still being typed */
static std::deque<std::string> console_queue;

static void console_line(const std::string &line)
{
    console_queue.push_back(line);
}

static void console_pump(void)
{
    if (console_queue.empty() || emu->paste_active()) return;
    emu->paste_text(console_queue.front(), 60, 60);
    console_queue.pop_front();
}

/* ---- the last machine used, kept in /ecat/<family>.last ---- */

static bool last_machine_read(unsigned *m)
{
    FIL f;
    char buf[8] = {0};
    UINT n = 0;
    if (f_open(&f, "/ecat/" ECAT_FAMILY ".last", FA_READ) != FR_OK) return false;
    f_read(&f, buf, sizeof(buf) - 1, &n);
    f_close(&f);
    unsigned v = (unsigned)atoi(buf);
    if (v >= ecat_machine_count) return false;
    *m = v;
    return true;
}

static void last_machine_write(unsigned m)
{
    FIL f;
    f_mkdir("/ecat");
    if (f_open(&f, "/ecat/" ECAT_FAMILY ".last", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    char buf[8];
    UINT n;
    snprintf(buf, sizeof(buf), "%u\n", m);
    f_write(&f, buf, strlen(buf), &n);
    f_close(&f);
}

/* ---- the settings, kept in /ecat/frank.ini ---- */

ecat_settings_t ecat_settings;

void ecat_settings_defaults(void)
{
    ecat_settings.audio_out = 0;            /* the menu's 0 HDMI, 1 I2S */
    ecat_settings.volume = 50;
    ecat_settings.muted = 0;
    ecat_settings.splash = 1;
}

static void settings_read(void)
{
    ecat_settings_defaults();
    FIL f;
    char buf[256] = {0};
    UINT n = 0;
    if (f_open(&f, "/ecat/frank.ini", FA_READ) != FR_OK) return;
    f_read(&f, buf, sizeof(buf) - 1, &n);
    f_close(&f);
    char *p = buf, *ln;
    while ((ln = strtok_r(p, "\r\n", &p)) != nullptr) {
        char *eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = ln, *v = eq + 1;
        if (!strcmp(k, "audio")) ecat_settings.audio_out = !strcmp(v, "i2s");
        else if (!strcmp(k, "volume")) { int x = atoi(v); ecat_settings.volume = (uint8_t)(x < 0 ? 0 : x > 100 ? 100 : x); }
        else if (!strcmp(k, "muted")) ecat_settings.muted = atoi(v) != 0;
        else if (!strcmp(k, "model_list")) ecat_settings.splash = atoi(v) != 0;
    }
}

bool ecat_settings_save(void)
{
    FIL f;
    UINT n;
    f_mkdir("/ecat");
    if (f_open(&f, "/ecat/frank.ini", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    char t[128];
    snprintf(t, sizeof(t), "audio=%s\nvolume=%u\nmuted=%u\nmodel_list=%u\n",
             ecat_settings.audio_out ? "i2s" : "hdmi", ecat_settings.volume,
             ecat_settings.muted, ecat_settings.splash);
    const bool ok = f_write(&f, t, strlen(t), &n) == FR_OK && n == strlen(t);
    return f_close(&f) == FR_OK && ok;
}

void ecat_settings_apply(void)
{
    const frank_audio_out_t out = ecat_settings.audio_out ? FRANK_AUDIO_I2S : FRANK_AUDIO_HDMI;
    if (frank_audio_output() != out) frank_audio_select(out);
    if (emu) {
        emu->set_volume(ecat_settings.volume);
        emu->set_muted(ecat_settings.muted != 0);
    }
}

/* ---- the machines, by English names ----
 *
 * The configurations name the machines in Russian (the system block's version,
 * or its name); the menu is English and its font Latin, so the names are
 * transliterated, the few with an established spelling taken from this table */

static std::string config_field(unsigned i, const char *key)
{
    const ecat_vfs_entry_t *v = ecat_vfs_find(ecat_machines[i]);
    if (!v) return std::string();
    const std::string t((const char *)v->data, v->size);
    const size_t sys = t.find("system");
    if (sys == std::string::npos) return std::string();
    const size_t end = t.find('}', sys);
    for (size_t k = t.find(key, sys); k != std::string::npos && k < end; k = t.find(key, k + 1)) {
        const size_t eq = t.find('=', k), nl = t.find('\n', k);
        if (eq == std::string::npos || eq > nl || (k && t[k - 1] != '\t' && t[k - 1] != ' ' && t[k - 1] != '\n'))
            continue;
        std::string name = t.substr(k, eq - k);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
        if (name != key) continue;
        std::string val = t.substr(eq + 1, nl - eq - 1);
        while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
        while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) val.pop_back();
        return val;
    }
    return std::string();
}

static std::string translit(const std::string &u)
{
    static const char *const lower[32] = {
        "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
        "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya",
    };
    std::string o;
    for (size_t i = 0; i < u.size(); i++) {
        const unsigned char c = (unsigned char)u[i];
        if (c < 0x80) { o += (char)c; continue; }
        if ((c & 0xE0) != 0xC0 || i + 1 >= u.size()) {
            while (i + 1 < u.size() && ((unsigned char)u[i + 1] & 0xC0) == 0x80) i++;
            continue;
        }
        const unsigned cp = ((c & 0x1F) << 6) | ((unsigned char)u[++i] & 0x3F);
        if (cp == 0x401 || cp == 0x451) { o += cp == 0x401 ? "Yo" : "yo"; continue; }
        if (cp < 0x410 || cp > 0x44F) continue;
        const bool upper = cp < 0x430;
        std::string s = lower[(cp - 0x410) & 31];
        if (upper && !s.empty()) {
            /* a capital in a run of capitals stays all capitals (РК -> RK) */
            const bool run = (i + 2 < u.size() && (unsigned char)u[i + 1] == 0xD0
                              && (unsigned char)u[i + 2] >= 0x90 && (unsigned char)u[i + 2] <= 0xAF)
                          || (!o.empty() && isupper((unsigned char)o.back()));
            for (size_t k = 0; k < s.size(); k++) if (run || k == 0) s[k] = (char)toupper((unsigned char)s[k]);
        }
        o += s;
    }
    return o;
}

static const struct { const char *ru, *en; } kNames[] = {
    { "Апогей БК-01Ц", "Apogey BK-01C" },
    { "Апогей-БК01Ц", "Apogey BK-01C" },
    { "Орион-128", "Orion-128" },
    { "Ириша", "Irisha" },
    { "Агат", "Agat" },
    { "Правец", "Pravetz" },
};

static std::string english(std::string s)
{
    for (const auto &n : kNames) {
        const size_t at = s.find(n.ru);
        if (at != std::string::npos) s.replace(at, strlen(n.ru), n.en);
    }
    return translit(s);
}

static std::vector<std::string> machine_titles;
static std::string family_title;

static void machine_names_build(void)
{
    for (unsigned i = 0; i < ecat_machine_count; i++) {
        std::string t = config_field(i, "version");
        if (t.empty()) t = config_field(i, "name");
        if (t.empty()) t = ecat_machines[i];
        machine_titles.push_back(english(t));
    }
    family_title = english(config_field(0, "name"));
}

/* ---- the menus' screen: the surface, put aside while they have it ---- */

static uint8_t surface_saved[sizeof(surface)] __attribute__((section(".psram_bss")));
static uint8_t ui_back[VGA_UI_W * VGA_UI_H] __attribute__((section(".psram_bss")));
uint8_t *vga_ui_fb = ui_back;
static vga_mode_t vga_mode = VGA_MODE_MACHINE;
static int saved_w, saved_h;

/* frank-dino's colour roles (ui_gfx.h), in the colours of a Soviet manual:
 * cream paper, black print, a red title band with a gold star, amber for
 * where you are */
static const uint32_t kUiPalette[] = {
    0x8C8270,   // C_BG          the desk round the window
    0xF3EBD8,   // C_PANEL       paper
    0xE4D8BC,   // C_PANEL_ALT   bands: the path, a dialog
    0x2B2118,   // C_SEP         rules and frames
    0x1E1A16,   // C_TEXT
    0x6B6257,   // C_TEXT_DIM
    0x000000,   // C_WHITE       emphasis: black on the paper
    0xF2B632,   // C_SEL_BG      the amber bar
    0xD9CBAE,   // C_SEL_BAND    the bar in the pane without the focus
    0xA51C16,   // C_ACCENT      red
    0xD8CCB2,   // C_FOOT_BG
    0x5A5248,   // C_SHADOW
    0xB01810,   // C_ICON_R
    0xA86A00,   // C_ICON_Y
    0x1F6F78,   // C_ICON_C
    0xA39A8C,   // C_DISABLED
    0xFFC83A,   // C_FLASH_R     the star and the first stripe: gold
    0xF08A24,   // C_FLASH_Y     orange
    0xF3E6C4,   // C_FLASH_G     cream
    0x6E0E0A,   // C_FLASH_C     crimson
    0xA51C16,   // C_TITLE_BG    the red band
    0xFFF4DC,   // C_TITLE_TEXT
    0x2E7D32,   // C_OK
};

void vga_build_ui_palette(void)
{
    for (unsigned i = 0; i < sizeof(kUiPalette) / sizeof(kUiPalette[0]); i++)
        graphics_set_palette(UI_PAL_BASE + i, kUiPalette[i]);
}

vga_mode_t vga_get_mode(void) { return vga_mode; }

void vga_ui_present(void)
{
    if (vga_mode != VGA_MODE_UI || ui_dirty_hi < ui_dirty_lo) return;
    const int lo = ui_dirty_lo, hi = ui_dirty_hi;
    ui_dirty_lo = VGA_UI_H;
    ui_dirty_hi = -1;
    /* Right after a vertical sync the beam is at the top: a copy of the
     * rows takes well under a frame (PSRAM to SRAM, ~0.5 ms the screen),
     * so it is done before the beam reaches what it changes, or after */
    const uint32_t v = vsyncs;
    const uint64_t t0 = time_us_64();
    while (vsyncs == v && time_us_64() - t0 < 25000) tight_loop_contents();
    memcpy(surface + lo * VGA_UI_W, ui_back + lo * VGA_UI_W, (size_t)(hi - lo + 1) * VGA_UI_W);
}

void vga_set_mode(vga_mode_t m)
{
    /* the surface is the menu's: core 1 stops drawing the machine on it */
    if (m == VGA_MODE_UI) {
        core1_render_hold = true;
        __dmb();
        while (core1_rendering) tight_loop_contents();
    }
    if (m == vga_mode) return;
    int hs, vs;
    if (m == VGA_MODE_UI) {
        saved_w = surface_w;
        saved_h = surface_h;
        memcpy(surface_saved, surface, sizeof(surface));
        vga_build_ui_palette();
        memset(surface, UI_PAL_BASE, VGA_UI_W * VGA_UI_H);
        memset(ui_back, UI_PAL_BASE, sizeof(ui_back));
        ui_dirty_lo = VGA_UI_H;
        ui_dirty_hi = -1;
        surface_w = VGA_UI_W;
        surface_h = VGA_UI_H;
        layout_for(VGA_UI_W, VGA_UI_H, &hs, &vs);
        hdmi_set_layout(VGA_UI_W, VGA_UI_H, hs, vs);
    } else {
        memcpy(surface, surface_saved, sizeof(surface));
        surface_w = saved_w;
        surface_h = saved_h;
        layout_for(saved_w, saved_h, &hs, &vs);
        hdmi_set_layout(saved_w, saved_h, hs, vs);
    }
    vga_mode = m;
    if (m == VGA_MODE_MACHINE) core1_render_hold = false;
}

/* ---- what the menus ask of the emulator (ui/ecat_ui.h) ---- */

static bool card_mounted;
static unsigned machine_running;
static bool booting = true;             /* the power-on model list: no machine yet */
static int boot_choice = -1;

/* a key the menu reads, as the console would send it */
static void menu_keys(const char *s)
{
    while (*s) menu_key(*s++);
}

static void hid_to_menu(uint8_t u)
{
    switch (u) {
    case 0x52: menu_keys("\x1b[A"); return;
    case 0x51: menu_keys("\x1b[B"); return;
    case 0x4F: menu_keys("\x1b[C"); return;
    case 0x50: menu_keys("\x1b[D"); return;
    case 0x4A: menu_keys("\x1b[H"); return;
    case 0x4D: menu_keys("\x1b[F"); return;
    case 0x4B: menu_keys("\x1b[5~"); return;
    case 0x4E: menu_keys("\x1b[6~"); return;
    case 0x28: case 0x58: menu_key('\r'); return;
    case 0x29: menu_key(0x1B); return;
    case 0x2A: menu_key(0x7F); return;
    case 0x2B: case 0x44: menu_key('\t'); return;     /* Tab, F11: close */
    case 0x45: menu_key('\\'); return;                /* F12: settings / close */
    case 0x3C: menu_key(0x06); return;                /* F3: find */
    default: break;
    }
    const int k = frank_hid_to_emukey(u);
    if (k >= 'A' && k <= 'Z') menu_key((char)(k - 'A' + 'a'));
    else if (k > 0 && k < 0x7F && k != '\\') menu_key((char)k);
}

extern "C" void ecat_ui_poll(void)
{
    vga_ui_present();
    platform_watchdog_feed();
    platform_crumb_tick();
    frank_audio_underruns = 0;          /* nothing plays while a menu is up */
    platform_usb_poll();
#if USB_HID_ENABLED
    uint8_t code;
    int down;
    while (usbhid_get_key_action(&code, &down))
        if (down) hid_to_menu(code);
#endif
}

extern "C" int ecat_ui_console_getchar(void)
{
    static bool in_cmd;
    const int c = getchar_timeout_us(0);
    if (c < 0) return -1;
    platform_uart_cmd(c);
    if (c == '~') { in_cmd = !in_cmd; return -1; }
    return in_cmd ? -1 : c;
}

extern "C" bool ecat_ui_card_mounted(void) { return card_mounted; }
extern "C" const char *ecat_ui_family_name(void) { return family_title.c_str(); }
extern "C" unsigned ecat_ui_machine_count(void) { return ecat_machine_count; }
extern "C" const char *ecat_ui_machine_title(unsigned i)
{
    return i < machine_titles.size() ? machine_titles[i].c_str() : "?";
}
extern "C" unsigned ecat_ui_machine_current(void) { return machine_running; }
extern "C" bool ecat_ui_booting(void) { return booting; }

extern "C" void ecat_ui_start_machine(unsigned i)
{
    if (booting) {
        boot_choice = (int)i;
        ui_request_close();
        return;
    }
    last_machine_write(i);
    boot_machine[0] = BOOT_MAGIC;
    boot_machine[1] = i;
    platform_reboot_clean(false);
}

extern "C" void ecat_ui_reset(bool cold)
{
    if (emu) emu->reset(cold);
}

extern "C" void ecat_ui_bootsel(void) { platform_reboot_clean(true); }

static std::string lower_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    std::string e = dot ? dot : "";
    for (char &c : e) c = (char)tolower((unsigned char)c);
    return e;
}

/* "*.rk *.rkr" (or a Qt filter list) names the extension */
static bool filter_has(const std::string &filter, const std::string &ext)
{
    if (ext.empty()) return false;
    std::string f = filter;
    for (char &c : f) c = (char)tolower((unsigned char)c);
    const std::string pat = "*" + ext;
    for (size_t at = f.find(pat); at != std::string::npos; at = f.find(pat, at + 1)) {
        const char after = at + pat.size() < f.size() ? f[at + pat.size()] : ' ';
        if (after == ' ' || after == ')' || after == ';') return true;
    }
    return false;
}

static TapeRecorder *machine_tape(void)
{
    if (!emu || !emu->dm) return nullptr;
    for (unsigned i = 0; i < emu->dm->device_count; i++) {
        TapeRecorder *t = dynamic_cast<TapeRecorder *>(emu->dm->get_device(i)->get());
        if (t) return t;
    }
    return nullptr;
}

static FDD *machine_drive(const std::string &ext)
{
    if (!emu || !emu->dm) return nullptr;
    for (unsigned i = 0; i < emu->dm->device_count; i++) {
        FDD *d = dynamic_cast<FDD *>(emu->dm->get_device(i)->get());
        if (d && filter_has(d->files, ext)) return d;
    }
    return nullptr;
}

static bool is_quickload(const std::string &ext)
{
    static const char *const known[] = { ".hex", ".rk", ".rkr", ".rkm", ".rka", ".gam", ".rko", ".ord", ".bru", ".cim", ".bin" };
    for (const char *k : known) if (ext == k) return true;
    return false;
}

extern "C" int ecat_ui_file_kind(const char *name)
{
    if (!emu) return FILE_OTHER;
    const std::string ext = lower_ext(name);
    if (is_quickload(ext) && filter_has(emu->get_system_data()->allowed_files, ext)) return FILE_PROGRAM;
    if (TapeRecorder *t = machine_tape())
        if (filter_has(t->files, ext) || (ext.size() > 1 && !t->format_for_extension(ext.substr(1)).empty()))
            return FILE_TAPE;
    if (machine_drive(ext)) return FILE_DISK;
    return FILE_OTHER;
}

extern "C" const char *ecat_ui_software_dir(void)
{
    /* the machine's own folder (its system type, as the desktop's library
     * names them), else the family's, else the library */
    static char dir[96];
    FILINFO fi;
    if (emu) {
        snprintf(dir, sizeof(dir), "/ecat/software/%s", emu->get_system_data()->system_type.c_str());
        if (f_stat(dir, &fi) == FR_OK) return dir;
    }
    snprintf(dir, sizeof(dir), "/ecat/software/%s", ECAT_FAMILY);
    if (f_stat(dir, &fi) == FR_OK) return dir;
    return f_stat("/ecat/software", &fi) == FR_OK ? "/ecat/software" : "/";
}

static void detail_of(const emulator::Result &r, const char *ok, const char *name, char *detail, unsigned n)
{
    if (r) { snprintf(detail, n, "%s %s", ok, name); return; }
    /* the core's messages are "{Context|text} more": the text and the rest */
    std::string m = r.message;
    const size_t bar = m.find('|'), close = m.find('}');
    if (bar != std::string::npos && close != std::string::npos && close > bar)
        m = m.substr(bar + 1, close - bar - 1) + m.substr(close + 1);
    snprintf(detail, n, "%s", m.c_str());
}

extern "C" bool ecat_ui_load(const char *path, int kind, char *detail, unsigned n)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    emulator::Result r = emulator::Result::ok();
    if (kind == FILE_PROGRAM) {
        r = HandleExternalFile(emu, path);
        detail_of(r, "Loaded", base, detail, n);
    } else if (kind == FILE_TAPE) {
        TapeRecorder *t = machine_tape();
        r = t->send_command("load", path);
        if (r) r = t->send_command("play", "");
        detail_of(r, "On the tape:", base, detail, n);
    } else if (kind == FILE_DISK) {
        FDD *d = machine_drive(lower_ext(path));
        r = d->send_command("load", path);
        detail_of(r, "In the drive:", base, detail, n);
    } else {
        snprintf(detail, n, "This machine cannot open %s", base);
        return false;
    }
    printf("ui load %s: %s\n", path, detail);
    return (bool)r;
}

extern "C" const char *ecat_ui_tape_state(void)
{
    TapeRecorder *t = machine_tape();
    if (!t) return nullptr;
    static char buf[UI_DYN_VALUE_LEN];
    const std::string &name = t->get_loaded_name();
    const int total = t->get_total(), pos = t->get_position();
    if (name.empty()) snprintf(buf, sizeof(buf), "empty");
    else if (total > 0) snprintf(buf, sizeof(buf), "%d%% %s", pos * 100 / total, name.c_str());
    else snprintf(buf, sizeof(buf), "%s", name.c_str());
    return buf;
}

extern "C" bool ecat_ui_tape_command(const char *command)
{
    TapeRecorder *t = machine_tape();
    return t && (bool)t->send_command(command, "");
}

static uint32_t speed_khz;
static bool speed_skip;

extern "C" const char *ecat_ui_speed(void)
{
    static char buf[UI_DYN_VALUE_LEN];
    if (!speed_khz) return "measuring";
    snprintf(buf, sizeof(buf), "%lu.%03lu MHz", (unsigned long)(speed_khz / 1000), (unsigned long)(speed_khz % 1000));
    return buf;
}

extern "C" void ecat_ui_info(char *buf, unsigned n)
{
    const video_mode_t m = graphics_get_video_mode(ECAT_VIDEO_MODE);
    const char *tape = ecat_ui_tape_state();
    snprintf(buf, n,
        UI_MK_HEAD "--- Machine ---\n"
        UI_MK_TEXT "Model          : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "Family         : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "Configuration  : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "CPU speed      : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "Screen         : " UI_MK_WHITE "%dx%d, %u colours\n"
        "\n"
        UI_MK_HEAD "--- Board ---\n"
        UI_MK_TEXT "Video          : " UI_MK_WHITE "%dx%d HDMI\n"
        UI_MK_TEXT "Sound          : " UI_MK_WHITE "%s, %d Hz\n"
        UI_MK_TEXT "Card           : %s\n"
        UI_MK_TEXT "Tape           : " UI_MK_WHITE "%s\n"
        UI_MK_TEXT "System clock   : " UI_MK_WHITE "%lu MHz\n"
        "\n"
        UI_MK_HEAD "--- Firmware ---\n"
        UI_MK_TEXT "frank-ecat     : " UI_MK_WHITE "%s, %s\n"
        UI_MK_TEXT "Emulator core  : " UI_MK_WHITE "eCat3 by Mikhail Revzin\n",
        ecat_ui_machine_title(machine_running), family_title.c_str(), ecat_machines[machine_running],
        ecat_ui_speed(), saved_w, saved_h, renderer ? renderer->palette_count() : 0,
        m.screen_width * 2, m.v_active,
        frank_audio_output() == FRANK_AUDIO_I2S ? "I2S" : "HDMI", FRANK_AUDIO_RATE,
        card_mounted ? UI_MK_OK "mounted" : UI_MK_BAD "no card",
        tape ? tape : "none",
        (unsigned long)(clock_get_hz(clk_sys) / 1000000),
        ECAT_VERSION, ECAT_FAMILY);
}

/* F11 the files, F12 the settings; the machine stands still meanwhile */
static void open_menu(bool settings)
{
    printf("menu: %s\n", settings ? "settings" : "files");
    menu_open_settings(settings);
    menu_run();
    speed_skip = true;
    printf("menu closed\n");
}

static void audio_output(frank_audio_out_t out)
{
    ecat_settings.audio_out = out == FRANK_AUDIO_I2S;
    ecat_settings_apply();
    ecat_settings_save();
}

int main()
{
    unsigned machine = 0;
    bool chosen = false, menu = false;
    if (boot_machine[0] == BOOT_MAGIC && boot_machine[1] < ecat_machine_count) {
        machine = boot_machine[1];
        chosen = true;
    } else if (boot_machine[0] == MENU_BOOT_MAGIC) {
        menu = true;
    }
    boot_machine[0] = 0;

    platform_init_clocks();
#if ECAT_CONSOLE_USB
    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) sleep_ms(100);
#endif
    platform_report_previous_crash();
    platform_safe_mode_if_hung();      /* the last run hung: report, BOOTSEL */
    platform_watchdog_start(5000);
    platform_stall_watch_start();
    platform_crumb(1, 0);
    platform_init_psram();
    platform_crumb(2, 0);
    card_mounted = platform_mount_sd() == 0;
    frank_fs_set_sd(card_mounted);
    platform_init_usb();
    platform_watchdog_feed();

    platform_crumb(3, 0);
    graphics_set_sys_clk_mhz(clock_get_hz(clk_sys) / 1000000);
    graphics_set_refresh(ECAT_VIDEO_MODE, ECAT_REFRESH);
    hdmi_lut_clear();
    renderer = new FrankRenderer();
    renderer->install_black();
    hdmi_set_layout(surface_w, surface_h, 2, 2);   /* until the machine says */
    platform_launch_core1(core1_main);
    multicore_fifo_pop_blocking();
    settings_read();
    frank_audio_init(ecat_settings.audio_out ? FRANK_AUDIO_I2S : FRANK_AUDIO_HDMI);
    machine_names_build();

    unsigned last_used = 0;
    const bool have_last = last_machine_read(&last_used);
    if (!chosen) machine = have_last ? last_used : 0;
    machine_running = machine;
    if (!chosen && (menu || ecat_settings.splash)) {
        /* The models, in the settings menu, before anything starts: on the
         * marked one after 3 s unless a key is pressed (at once on
         * "~C m~", which waits) */
        while (getchar_timeout_us(0) >= 0) {}      /* what the console had */
        menu_autoclose(menu ? 0 : 3000);
        menu_preload("\r\r");          /* Machine, then Model */
        menu_open_settings(true);
        menu_run();
        if (boot_choice >= 0) machine = (unsigned)boot_choice;
    }
    booting = false;
    machine_running = machine;
    if (!have_last || last_used != machine) last_machine_write(machine);

    printf("machines:\n");
    for (unsigned i = 0; i < ecat_machine_count; i++) printf("  %u%s %s\n", i, i == machine ? "*" : " ", ecat_machines[i]);

    platform_watchdog_feed();
    platform_crumb(4, machine);
    emu = new Emulator("/ecat/computers/", "/ecat/data/", "/ecat/software/", "/ecat/ecat.ini", renderer);
    platform_crumb(5, machine);
    ecat_trace_throws = true;
    emulator::Result res = emu->load_config(ecat_machines[machine]);
    ecat_trace_throws = false;
    platform_watchdog_feed();
    if (!res) {
        printf("load_config %s: %s\n", ecat_machines[machine], res.message.c_str());
    } else {
        platform_crumb(6, machine);
        ecat_settings_apply();
        emu->init_video(nullptr);       /* the display gets the renderer, as in the console build */
        emu->run();
        /* A terminal with a processor of its own (the ДВК): on the second core */
        ecat_core_num = core_num;
        ecat_time_us = time_us_64;
        if (emu->set_parallel({"ie15-cpu", "ksm-cpu", "kcgd-cpu", "kmd-cpu"})) {
            par_emu = emu;
            core1_renders = emu->display_on_core1();
            /* (The КМД's own ВМ1 stays on core 0: on core 1 the HDMI line IRQ
             * there came late during its disk transfers, and the scan-out
             * stopped for good - not understood yet.)
             * The ИЕ15 terminal is the heavier half: its core gets nothing else
             * (the КСМ and КЦГД ones are lighter than the ДВК-3/4 main side) */
            if (emu->dm->get_device_by_name("ie15-cpu", false) != nullptr) hdmi_irq_to_core0();
        }
        printf("running %s\n", ecat_machines[machine]);
    }
    platform_watchdog_feed();
    ecat_heap_report();

    uint64_t last = time_us_64(), last_status = last;
    uint64_t busy_us = 0;
    std::string line;
    while (true) {
        /* Slices of 1 ms at least, as the emulation loop of run() does:
         * slice_ticks() rounds each slice down to whole cycles, and many tiny
         * slices lost ~5% of the machine's time that way */
        while (time_us_64() - last < 1000) tight_loop_contents();
        const uint64_t now = time_us_64();
        platform_watchdog_feed();
        platform_crumb_tick();
        platform_crumb(10, (uint32_t)(now - last));
        /* At most 20 ms of machine time a slice: a machine slower than real time
         * runs in slow motion instead of asking for ever longer slices */
        emu->slice((now - last) > 20000 ? 20000 : (now - last));
        last = now;
        platform_crumb(11, 0);
        if (!core1_renders) emu->render_tick();
        platform_crumb(12, 0);
        audio_pump();
        /* The scan-out stopped (no vsync for 100 ms): set up again, by the core
         * whose IRQ it is */
        {
            static uint32_t seen_vs;
            static uint64_t seen_at;
            if (vsyncs != seen_vs) { seen_vs = vsyncs; seen_at = now; }
            else if (now - seen_at > (hdmi_restarts ? 2000000u : 100000u)) {
                hdmi_report_state();
                if (!hdmi_restarts) hdmi_report_late();
                hdmi_restarts++;
                printf("hdmi: scan-out stopped, restarting (%lu)\n", (unsigned long)hdmi_restarts);
                if (hdmi_irq_core == 0) hdmi_restart();
                else hdmi_restart_req = true;
                seen_at = now;
            }
        }
        busy_us += time_us_64() - now;
        platform_crumb(13, 0);
        platform_usb_poll();
        platform_crumb(14, 0);
#if USB_HID_ENABLED
        {
            static int mods;
            uint8_t code;
            int down;
            while (usbhid_get_key_action(&code, &down)) {
                const int key = frank_hid_to_emukey(code);
                if (!key) continue;
                const int bit = key == EMUKEY_SHIFT ? EMUMOD_SHIFT : key == EMUKEY_CONTROL ? EMUMOD_CONTROL
                              : key == EMUKEY_ALT ? EMUMOD_ALT : 0;
                if (bit) mods = down ? (mods | bit) : (mods & ~bit);
                if (key == EMUKEY_F11 || key == EMUKEY_F12) {   /* files, settings */
                    if (down) {
                        open_menu(key == EMUKEY_F12);
                        mods = 0;
                        last = time_us_64();
                    }
                    continue;
                }
                emu->key_event(key, mods, down != 0, true);
            }
        }
#endif

        /* every byte the console has: a slow machine goes round this loop ten
         * times a second, and a "~BOOT~" sent by a script that then closes
         * the port was lost a byte at a time */
        for (int c; (c = getchar_timeout_us(0)) >= 0;) {
            platform_uart_cmd(c);
            /* "~...~" are console commands (platform_uart_cmd), not typing */
            static bool in_cmd;
            if (c == '~') {
                in_cmd = !in_cmd;
            } else if (in_cmd) {
            } else if (c == '\r' || c == '\n') {
                console_line(line + "\n");
                line.clear();
            } else if (c >= 32) {
                line += (char)c;
            }
        }
        char cmd[96];
        if (platform_debug_cmd_line(cmd, sizeof(cmd))) {
            unsigned n;
            if (strcmp(cmd, "m") == 0 || strcmp(cmd, "menu") == 0 || strcmp(cmd, "files") == 0) {
                open_menu(cmd[0] != 'f');       /* driven from here: ESC [ A and so on */
                last = time_us_64();
            } else if (strcmp(cmd, "boot") == 0) {
                boot_machine[0] = MENU_BOOT_MAGIC;
                printf("model list, rebooting\n");
                sleep_ms(20);
                platform_reboot_clean(false);
            } else if (sscanf(cmd, "m %u", &n) == 1 && n < ecat_machine_count) {
                boot_machine[0] = BOOT_MAGIC;
                boot_machine[1] = n;
                printf("machine %u: %s, rebooting\n", n, ecat_machines[n]);
                sleep_ms(20);
                platform_reboot_clean(false);
            } else if (cmd[0] == 's') {
                status();
            } else if (cmd[0] == 'd') {
                /* what the machine shows and runs: surface, palette, CPU */
                unsigned nz = 0;
                for (int i = 0; i < surface_w * surface_h; i++) nz += surface[i] != 0;
                printf("surface %dx%d, %u dots not 0, palette %u colours\n", surface_w, surface_h, nz,
                       renderer->palette_count());
                CPU *c = dynamic_cast<CPU *>(emu->dm->get_device_by_name("cpu", false));
                if (c && c->mm) {
                    printf("cpu pc %04x, F800:", c->get_pc());
                    for (unsigned a = 0xF800; a < 0xF810; a++) printf(" %02x", c->mm->read(a) & 0xFF);
                    printf(", 7600:");
                    for (unsigned a = 0x7600; a < 0x7610; a++) printf(" %02x", c->mm->read(a) & 0xFF);
                    printf("\n");
                }
            } else if (cmd[0] == 'c' && cmd[1] == ' ') {
                /* "c DEVICE COMMAND PARAMETERS": a device command, as the
                 * scripts' COMMAND device.command(parameters) */
                char dev[32] = "", com[32] = "";
                int used = 0;
                if (sscanf(cmd + 2, "%31s %31s %n", dev, com, &used) >= 2) {
                    ComputerDevice *d = emu->dm->get_device_by_name(dev, false);
                    if (!d) printf("no device %s\n", dev);
                    else {
                        emulator::Result r = d->send_command(com, std::string(cmd + 2 + used));
                        printf("%s.%s: %s\n", dev, com, r ? "ok" : r.message.c_str());
                    }
                }
            } else if (cmd[0] == 'f' && cmd[1] == ' ') {
                /* "f DEVICE FIELD...": device fields, as the scripts' LOG */
                char dev[32] = "";
                int used = 0;
                if (sscanf(cmd + 2, "%31s %n", dev, &used) >= 1) {
                    ComputerDevice *d = emu->dm->get_device_by_name(dev, false);
                    if (!d) printf("no device %s\n", dev);
                    char *p = cmd + 2 + used, *tok;
                    while (d && (tok = strtok_r(p, " ", &p)) != nullptr) {
                        DeviceFieldValue v;
                        if (!d->get_field(tok, 0, 0, v)) { printf("%s.%s: ?\n", dev, tok); continue; }
                        printf("%s.%s:", dev, tok);
                        if (v.numeric) for (unsigned x : v.values) printf(" %u", x);
                        else printf(" %s", v.text.c_str());
                        printf("\n");
                    }
                }
            } else if (strncmp(cmd, "put ", 4) == 0) {
                char path[200];
                unsigned size;
                if (sscanf(cmd + 4, "%199s %u", path, &size) == 2) console_put(path, size);
            } else if (strncmp(cmd, "load ", 5) == 0) {
                const std::string path(cmd + 5);
                emulator::Result r = HandleExternalFile(emu, path);
                printf("load %s: %s\n", path.c_str(), r ? "ok" : r.message.c_str());
            } else if (strcmp(cmd, "audio hdmi") == 0) {
                audio_output(FRANK_AUDIO_HDMI);
            } else if (strcmp(cmd, "audio i2s") == 0) {
                audio_output(FRANK_AUDIO_I2S);
            } else if (strncmp(cmd, "prof1", 5) == 0) {
                unsigned us = 200;
                sscanf(cmd + 5, "%u", &us);
                platform_prof_start_core1(us);
            } else if (strncmp(cmd, "prof", 4) == 0) {
                unsigned us = 200;
                sscanf(cmd + 4, "%u", &us);
                platform_prof_start(us);
            }
        }
        console_pump();
        platform_prof_dump_if_done();
        if (now - last_status >= 5000000) {
            static uint64_t last_clock;
            const uint64_t clk = emu->clock_now();
            const uint64_t hz = (clk - last_clock) * 1000000 / (now - last_status);
            last_clock = clk;
            if (!speed_skip) speed_khz = (uint32_t)(hz / 1000);    /* a menu stopped the machine */
            speed_skip = false;
            if (emu->parallel()) {
                printf("parallel: first core waited %lu us of %lu, %lu quanta\n", (unsigned long)emu->m_par_wait_us,
                       (unsigned long)(now - last_status), (unsigned long)emu->m_par_quanta);
                emu->m_par_wait_us = emu->m_par_quanta = 0;
            }
            printf("speed %lu.%02lu MHz, busy %u%%, vsync %lu, ring %d, underruns %lu\n",
                   (unsigned long)(hz / 1000000), (unsigned long)(hz / 10000 % 100),
                   (unsigned)(busy_us * 100 / (now - last_status)), (unsigned long)vsyncs,
                   frank_audio_level(), (unsigned long)frank_audio_underruns);
            last_status = now;
            busy_us = 0;
        }
    }
}
