/*
 * frank-ecat — eCat3 on RP2350 (FRANK M2)
 *
 * The pieces of pico-speccy's vga-nextgen/vga.c that its HDMI driver
 * (hdmi.c) relies on, for an HDMI-only build (no VGA_HDMI): the frame
 * placement state, graphics_init() and the setters that only record values.
 *
 * The application provides getLineBuffer(), get_video_mode(),
 * ESPectrum_vsync() and tryCalloc() (see hdmi.c).
 */

#include "graphics.h"

bool SELECT_VGA = false;
enum graphics_mode_t graphics_mode = GRAPHICSMODE_DEFAULT;
int graphics_buffer_width = 0;
int graphics_buffer_height = 0;
int graphics_buffer_shift_x = 0;
int graphics_buffer_shift_y = 0;
volatile bool profi_ds80_active = false;

static bool is_flash_line, is_flash_frame;

void graphics_init_hdmi(void);
void graphics_set_bgcolor_hdmi(uint32_t color888);

/* on the core that is to run the line IRQ */
void graphics_init(void) {
    graphics_init_hdmi();
}

void graphics_set_mode(enum graphics_mode_t mode) {
    graphics_mode = mode;
}

void graphics_set_buffer(uint8_t *buffer, uint16_t width, uint16_t height) {
    (void)buffer; /* the driver pulls rows through getLineBuffer() */
    graphics_buffer_width = width;
    graphics_buffer_height = height;
}

void graphics_set_offset(int x, int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

void graphics_set_bgcolor(uint32_t color888) {
    graphics_set_bgcolor_hdmi(color888);
}

void graphics_set_flashmode(bool flash_line, bool flash_frame) {
    is_flash_line = flash_line;
    is_flash_frame = flash_frame;
}

void graphics_set_textbuffer(uint8_t *buffer) {
    (void)buffer;
}

void clrScr(uint8_t color) {
    (void)color;
}
