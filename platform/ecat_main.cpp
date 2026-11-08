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
#include "platform.h"
#include "frank_audio.h"
#include "frank_vfs.h"
#include "graphics.h"
#include "hdmi.h"
#include "ff.h"
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

/* the desktop's cache of unpacked extension files (emulator/cache.cpp, not
 * built here: the firmware's extensions are flattened at build time) */
void cache_touch(const std::string &) {}

/* ---- core 1: the HDMI line IRQ ---- */

static void core1_main(void)
{
    graphics_init();
    graphics_set_bgcolor(0x000000);
    multicore_fifo_push_blocking(1);
    while (1) tight_loop_contents();
}

/* the machine to boot, kept over a watchdog reboot */
static uint32_t __attribute__((section(".uninitialized_data"))) boot_machine[2];
#define BOOT_MAGIC 0xEC47B007u

static FrankRenderer *renderer;
static Emulator *emu;

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

int main()
{
    unsigned machine = 0;
    if (boot_machine[0] == BOOT_MAGIC && boot_machine[1] < ecat_machine_count) machine = boot_machine[1];
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
    frank_fs_set_sd(platform_mount_sd() == 0);
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
    frank_audio_init(FRANK_AUDIO_HDMI);

    printf("machines:\n");
    for (unsigned i = 0; i < ecat_machine_count; i++) printf("  %u%s %s\n", i, i == machine ? "*" : " ", ecat_machines[i]);

    platform_watchdog_feed();
    platform_crumb(4, machine);
    emu = new Emulator("/ecat/computers/", "/ecat/data/", "/ecat/software/", "/ecat/ecat.ini", renderer);
    platform_crumb(5, machine);
    emulator::Result res = emu->load_config(ecat_machines[machine]);
    platform_watchdog_feed();
    if (!res) {
        printf("load_config %s: %s\n", ecat_machines[machine], res.message.c_str());
    } else {
        platform_crumb(6, machine);
        emu->set_volume(atoi(emu->read_setup("Sound", "volume", "50").c_str()));
        emu->set_muted(emu->read_setup("Sound", "muted", "0") == "1");
        emu->init_video(nullptr);       /* the display gets the renderer, as in the console build */
        emu->run();
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
        emu->render_tick();
        platform_crumb(12, 0);
        audio_pump();
        busy_us += time_us_64() - now;
        platform_crumb(13, 0);
        platform_usb_poll();
        platform_crumb(14, 0);

        int c = getchar_timeout_us(0);
        if (c >= 0) {
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
            if (sscanf(cmd, "m %u", &n) == 1 && n < ecat_machine_count) {
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
            printf("speed %lu.%02lu MHz, busy %u%%, vsync %lu, ring %d, underruns %lu\n",
                   (unsigned long)(hz / 1000000), (unsigned long)(hz / 10000 % 100),
                   (unsigned)(busy_us * 100 / (now - last_status)), (unsigned long)vsyncs,
                   frank_audio_level(), (unsigned long)frank_audio_underruns);
            last_status = now;
            busy_us = 0;
        }
    }
}
