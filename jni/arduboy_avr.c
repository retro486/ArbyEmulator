/*
	Copyright (C) 2018 OBONO

	Arduboy emulator using simavr on Android platform.

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sim_avr.h>
#include <avr_eeprom.h>
#include <avr_ioport.h>
#include <avr_extint.h>
#include <sim_hex.h>
#include <sim_time.h>
#include <ssd1306_virt.h>

#include "arduboy_avr.h"

#define MHZ_16 (16000000) // 16 MHz
#define REFRESH_PERIOD_US (16666)

#define RGB(r,g,b) (0xFF000000 | (uint8_t)(r) << 16 | (uint8_t)(g) << 8 | (uint8_t)(b))
#define BLACK RGB(0, 0, 0)

static struct button_info {
	enum button_e btn_id;
	avr_irq_t *irq;
	const char *name;
	char port_name;
	int port_idx;
	bool pressed;
} buttons[BTN_COUNT] = {
	{ BTN_UP, NULL, "btn.up", 'F', 7 },
	{ BTN_DOWN, NULL, "btn.down", 'F', 4 },
	{ BTN_LEFT, NULL, "btn.left", 'F', 5 },
	{ BTN_RIGHT, NULL, "btn.right", 'F', 6 },
	{ BTN_A, NULL, "btn.a", 'E', 6 },
	{ BTN_B, NULL, "btn.b", 'B', 4 },
};

/* SSD1306 wired to the SPI bus, with the following additional pins: */
static ssd1306_wiring_t ssd1306_wiring =
{
	.chip_select.port = 'D',
	.chip_select.pin = 6,
	.data_instruction.port = 'D',
	.data_instruction.pin = 4,
	.reset.port = 'D',
	.reset.pin = 7,
};

static struct arduboy_avr_mod_state {
	struct avr_t *avr;
	ssd1306_t ssd1306;
	bool yield;
	uint8_t lumamap[OLED_HEIGHT_PX][OLED_WIDTH_PX];
	int *pixels;
} mod_s;

/*------------------------------------------------------------------------------------------------*/

static void android_logger(avr_t * avr, const int level, const char * format, va_list ap)
{
	if (!avr || avr->log >= level) {
		int android_level = ANDROID_LOG_SILENT - level;
		__android_log_print(android_level, LOG_TAG, format, ap);
	}
}


static void update_lumamap(struct ssd1306_t *ssd1306)
{
	for (int p = 0; p < SSD1306_VIRT_PAGES; p++) {
		for (int c = 0; c < SSD1306_VIRT_COLUMNS; c++) {
			uint8_t px_col = ssd1306->vram[p][c];
			for (int y = 0; y < 8; y++) {
				mod_s.lumamap[p * 8 + y][c] = px_col & 0x1;
				px_col >>= 1;
			}
		}
	}
}

static inline int get_fg_colour(uint8_t invert, float opacity)
{
	return invert ? BLACK : RGB(255 * opacity, 255 * opacity, 255 * opacity);
}

static inline int get_bg_colour(uint8_t invert, float opacity)
{
	return invert ? get_fg_colour(0, opacity) : BLACK;
}

static inline float contrast_to_opacity(uint8_t contrast)
{
	// Typically the screen will be clearly visible even at 0 contrast
	return contrast / 512.0 + 0.5;
}

static void render_screen(struct ssd1306_t *ssd1306)
{
	if (!ssd1306_get_flag(ssd1306, SSD1306_FLAG_DISPLAY_ON)) {
		return;
	}

	// Apply vertical and horizontal display mirroring
	int orig_x = 0, orig_y = 0;
	int vx = 1, vy = 1;
	if (ssd1306_get_flag(ssd1306, SSD1306_FLAG_SEGMENT_REMAP_0)) {
		orig_x = OLED_WIDTH_PX - 1; vx = -1;
	}
	if (ssd1306_get_flag(ssd1306, SSD1306_FLAG_COM_SCAN_NORMAL)) {
		orig_y = OLED_HEIGHT_PX - 1; vy = -1;
	}
	
	// Setup drawing colour
	int invert = ssd1306_get_flag (ssd1306, SSD1306_FLAG_DISPLAY_INVERTED);
	float opacity = contrast_to_opacity(ssd1306->contrast_register);
	int bg_color = get_bg_colour(invert, opacity);
	int fg_color = get_fg_colour(invert, opacity);

	// Render screen
	int *pixels = mod_s.pixels;
	for (int y = orig_y; y >= 0 && y < OLED_HEIGHT_PX; y += vy) {
		for (int x = orig_x; x >= 0 && x < OLED_WIDTH_PX; x += vx) {
			*pixels++ = mod_s.lumamap[y][x] ? fg_color : bg_color;
		}
	}
}

/*------------------------------------------------------------------------------------------------*/

static void hook_ssd1306_write_data(struct avr_irq_t *irq, uint32_t value, void *param)
{
	ssd1306_t *ssd1306 = (ssd1306_t *) param;
	if (ssd1306->di_pin == SSD1306_VIRT_DATA) {
		if (ssd1306->cursor.page == 0 && ssd1306->cursor.column == 0 &&
				ssd1306_get_flag(ssd1306, SSD1306_FLAG_DIRTY)) {
			update_lumamap(ssd1306);
			ssd1306_set_flag(ssd1306, SSD1306_FLAG_DIRTY, 0);
		}
	}
}

static avr_cycle_count_t update_screen(
		avr_t *avr,
		avr_cycle_count_t when,
		void *param)
{
	render_screen(param);
	mod_s.yield = true;
	return avr->cycle + avr_usec_to_cycles(avr, REFRESH_PERIOD_US);
}

/*------------------------------------------------------------------------------------------------*/

int arduboy_avr_setup(const char *hex_file_path, int cpu_freq)
{
	avr_global_logger_set(android_logger);
	mod_s.avr = NULL;

	avr_t *avr = avr_make_mcu_by_name("atmega32u4");
	if (!avr) {
		LOGE("Failed to make AVR\n");
		return -1;
	}
	avr_init(avr);

	/*
	BTN_A is wired to INT6 which defaults to level triggered.
	This means that while button A is pressed the interrupt triggers
	continuously. This is very expensive to simulate so we set non-strict
	level trigger mode for INT6.

	Why doesn't this affect real h/w?
	*/
	avr_extint_set_strict_lvl_trig(avr, EXTINT_IRQ_OUT_INT6, 0);

	{
		/* Load .hex and setup program counter */
		uint32_t boot_base, boot_size;
		uint8_t * boot = read_ihex_file(hex_file_path, &boot_size, &boot_base);
		if (!boot) {
			avr_terminate(avr);
			LOGE("Unable to load \"%s\"\n", hex_file_path);
			return -1;
		}
		memcpy(avr->flash + boot_base, boot, boot_size);
		free(boot);
		avr->pc = boot_base;
		/* end of flash, remember we are writing /code/ */
		avr->codeend = avr->flashend;
	}

	/* more simulation parameters */
	avr->log = LOG_DEBUG; // LOG_NONE
	avr->frequency = cpu_freq;
	avr->run_cycle_limit = avr_usec_to_cycles(avr, REFRESH_PERIOD_US * 2);

	/* setup and connect display controller */
	ssd1306_t *ssd1306 = &mod_s.ssd1306;
	ssd1306_init(avr, ssd1306, OLED_WIDTH_PX, OLED_HEIGHT_PX);
	ssd1306_connect(ssd1306, &ssd1306_wiring);
	avr_irq_register_notify(ssd1306->irq + IRQ_SSD1306_SPI_BYTE_IN, hook_ssd1306_write_data, ssd1306);
	avr_irq_register_notify(ssd1306->irq + IRQ_SSD1306_TWI_OUT, hook_ssd1306_write_data, ssd1306);
	memset(mod_s.lumamap, 0, sizeof(mod_s.lumamap));

	/* setup and connect buttons */
	for (int btn_idx = 0; btn_idx < BTN_COUNT; btn_idx++) {
		struct button_info *binfo = &buttons[btn_idx];
		binfo->irq = avr_alloc_irq(&avr->irq_pool, 0, 1, &binfo->name);
		uint32_t iop_ctl = AVR_IOCTL_IOPORT_GETIRQ(binfo->port_name);
		avr_irq_t *iop_irq = avr_io_getirq(avr, iop_ctl, binfo->port_idx);
		avr_connect_irq(binfo->irq, iop_irq);
		/* pull up pin */
		avr_raise_irq(binfo->irq, 1);
	}

	/* Setup display render timers */
	avr_cycle_timer_register_usec(avr, REFRESH_PERIOD_US, update_screen, ssd1306);

	mod_s.avr = avr;
	LOGI("Setup AVR\n");
	return 0;
}

bool arduboy_avr_get_eeprom(char *p_array)
{
	if (!mod_s.avr) {
		return false;
	}
	struct mcu_t {
		avr_t		core;
		avr_eeprom_t	eeprom;
	} *mcu = (struct mcu_t *) &mod_s.avr;
	memcpy(p_array, mcu->eeprom.eeprom, mcu->eeprom.size);
	return true;
}

bool arduboy_avr_set_eeprom(const char *p_array)
{
	if (!mod_s.avr) {
		return false;
	}
	struct mcu_t {
		avr_t		core;
		avr_eeprom_t	eeprom;
	} *mcu = (struct mcu_t *) &mod_s.avr;
	memcpy(mcu->eeprom.eeprom, p_array, mcu->eeprom.size);
	return true;
}

void arduboy_avr_button_event(enum button_e btn_e, bool pressed)
{
	if (!mod_s.avr) {
		return;
	}
	struct button_info *btn = (btn_e < BTN_COUNT) ? &buttons[btn_e] : NULL;
	if (btn && btn->pressed != pressed) {
		avr_raise_irq(btn->irq, !pressed);
		btn->pressed = pressed;
	}
}

bool arduboy_avr_loop(int *pixels)
{
	avr_t *avr = mod_s.avr;
	if (!avr) {
		return false;
	}
	mod_s.yield = false;
	mod_s.pixels = pixels;
	while (!mod_s.yield) {
		int state = avr_run(avr);
		if (state == cpu_Done || state == cpu_Crashed) {
			return false;
		}
	}
	return true;
}


void arduboy_avr_teardown(void)
{
	if (mod_s.avr) {
		avr_terminate(mod_s.avr);
		mod_s.avr = NULL;
		LOGI("Terminate AVR\n");
	}
}
