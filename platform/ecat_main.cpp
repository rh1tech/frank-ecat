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
#include <string>

#include "emulator/emulator.h"
#include "emulator/audio/audio_driver.h"
#include "emulator/indexed_palette.h"

extern "C" {
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "platform.h"
#include "frank_audio.h"
#include "frank_vfs.h"
#include "graphics.h"
#include "hdmi.h"
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

/* ---- sound: the core's mixer pulled into the output ring ---- */

class FrankAudioDriver : public AudioDriver
{
public:
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

    /* main loop: as many chunks as the ring has room for */
    void pump()
    {
        if (!m_running || !m_callback) return;
        while (frank_audio_free() >= CHUNK) {
            int16_t in[CHUNK * 2], out[CHUNK * 2];
            m_callback(m_userdata, reinterpret_cast<uint8_t *>(in), CHUNK * m_channels * 2);
            const int16_t *lr = in;
            if (m_channels == 1) {
                for (int i = 0; i < CHUNK; i++) out[2 * i] = out[2 * i + 1] = in[i];
                lr = out;
            }
            frank_audio_push(lr, CHUNK);
        }
    }

private:
    enum { CHUNK = 256 };
    int m_channels = 1;
    AudioCallback m_callback = nullptr;
    void *m_userdata = nullptr;
    bool m_running = false;
};

static FrankAudioDriver *audio_driver;

AudioDriver *ecat_create_audio_driver()
{
    if (!audio_driver) audio_driver = new FrankAudioDriver();
    return audio_driver;
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
    printf("vsync %lu, audio ring %d, underruns %lu\n", (unsigned long)vsyncs, frank_audio_level(),
           (unsigned long)frank_audio_underruns);
}

static void console_line(const std::string &line)
{
    if (line.empty()) return;
    emu->paste_text(line, 60, 60);
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
    extern volatile bool ecat_malloc_trace;
    ecat_malloc_trace = true;
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
    ecat_malloc_trace = false;
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
        static unsigned iter;
        const bool trace = iter < 8;
        platform_watchdog_feed();
        platform_crumb_tick();
        platform_crumb(10, (uint32_t)(now - last));
        const uint64_t c0 = emu->clock_now();
        /* At most 20 ms of machine time a slice: a machine slower than real time
         * runs in slow motion instead of asking for ever longer slices */
        emu->slice((now - last) > 20000 ? 20000 : (now - last));
        const uint64_t t1 = time_us_64();
        if (trace) printf("[%u] asked %llu us: %llu cycles in %llu us\n", iter, (unsigned long long)(now - last),
                          (unsigned long long)(emu->clock_now() - c0), (unsigned long long)(t1 - now));
        last = now;
        platform_crumb(11, 0);
        emu->render_tick();
        const uint64_t t2 = time_us_64();
        platform_crumb(12, 0);
        if (audio_driver) audio_driver->pump();
        const uint64_t t3 = time_us_64();
        if (trace) printf("[%u] render %llu us, audio %llu us\n", iter, (unsigned long long)(t2 - t1),
                          (unsigned long long)(t3 - t2));
        iter++;
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
            } else if (strncmp(cmd, "prof", 4) == 0) {
                unsigned us = 200;
                sscanf(cmd + 4, "%u", &us);
                platform_prof_start(us);
            }
        }
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
