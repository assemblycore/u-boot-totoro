// SPDX-License-Identifier: GPL-2.0+
/* SBL-initialized BCM21553: UART console mirrored to LCDC PIO. */
#include <config.h>
#include <init.h>
#include <serial.h>
#include <time.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <video_font_data.h>
#include <video_font_8x16.h>
#include <command.h>
#include <env.h>
#include <stdio_dev.h>
#include "phone.h"
DECLARE_GLOBAL_DATA_PTR;
#define REG(a) (*(volatile u32 *)(a))
#define UART 0x08820000
#define LCD 0x08030000
static unsigned char cells[20][30];
static unsigned int row, col;
static bool lcd_silent;
static void lcd_write(unsigned int off, u32 value)
{
	while (!(REG(LCD + 0x1c) & (1U << 31))) {}
	REG(LCD + off) = value;
}
static void window(unsigned int cmd, unsigned int first, unsigned int last)
{
	REG(LCD + 0x18) &= ~0x80U;
	lcd_write(0, cmd);
	lcd_write(4, first >> 8); lcd_write(4, first & 255);
	lcd_write(4, last >> 8); lcd_write(4, last & 255);
}
static void glyph(unsigned int x, unsigned int y, unsigned char c)
{
	unsigned int i, j;
	window(0x2a, x * 8, x * 8 + 7);
	window(0x2b, y * 16, y * 16 + 15);
	lcd_write(0, 0x2c);
	REG(LCD + 0x18) |= 0x80;
	for (i = 0; i < 16; i++)
		for (j = 0; j < 8; j++)
			lcd_write(4, (video_fontdata_8x16[c * 16 + i] & (0x80 >> j)) ? 0xffffff : 0);
	while (!(REG(LCD + 0x1c) & (1U << 31))) {}
	REG(LCD + 0x18) &= ~0x80U;
}
static void newline(void)
{
	unsigned int x, y;
	col = 0;
	if (++row < 20) return;
	row = 19;
	memmove(cells[0], cells[1], 19 * 30);
	memset(cells[19], ' ', 30);
	for (y = 0; y < 20; y++)
		for (x = 0; x < 30; x++) glyph(x, y, cells[y][x]);
}
static int console_start(void)
{
	unsigned int x, y;
	/* Match the PIO protocol of the hardware-tested hello-cpu payload. */
	REG(LCD + 0x18) &= ~((1U << 31) | 0x80U);
	window(0x2a, 0, 239);
	window(0x2b, 0, 319);
	lcd_write(0, 0x2c);
	REG(LCD + 0x18) |= 0x80;
	for (y = 0; y < 320; y++)
		for (x = 0; x < 240; x++) lcd_write(4, 0);
	while (!(REG(LCD + 0x1c) & (1U << 31))) {}
	REG(LCD + 0x18) &= ~0x80U;
	row = col = 0;
	memset(cells, ' ', sizeof(cells));
	return 0;
}
static void uart_putc(unsigned char c)
{
	unsigned int timeout = 64;
	while (!(REG(UART + 20) & 0x20) && --timeout) {}
	if (timeout) REG(UART) = c;
}
static void console_putc(const char c)
{
	if (c == '\n') uart_putc('\r');
	uart_putc(c);
	if (lcd_silent) return;
	if (c == '\r') { col = 0; return; }
	if (c == '\n') { newline(); return; }
	if (c == '\b') { if (col) --col; return; }
	if ((unsigned char)c < 32) return;
	if (col == 30) newline();
	cells[row][col] = c; glyph(col++, row, c);
}
static void console_puts(const char *s) { while (*s) console_putc(*s++); }
static bool in_menu;
static bool home_cancel_armed;
static int console_tstc(void)
{
	bool home = !(REG(0x088ce0a0) & 0x10000);
	/* A Home press selecting USB must be released before it can cancel. */
	if (!home) home_cancel_armed = true;
	return !!(REG(UART + 20) & 1) ||
	       (!in_menu && home_cancel_armed && home);
}
static int console_getc(void)
{
	while (!console_tstc()) {}
	if (!(REG(UART + 20) & 1)) return 3; /* Home cancels USB waits. */
	return REG(UART) & 255;
}
static void console_setbrg(void) { /* Inherit the SBL UART divisor. */ }
static struct serial_device console = {
	.name = "serial", .start = console_start, .setbrg = console_setbrg,
	.putc = console_putc, .puts = console_puts, .getc = console_getc,
	.tstc = console_tstc,
};
struct serial_device *default_serial_console(void) { return &console; }
void ns16550_serial_initialize(void) { serial_register(&console); }
int board_init(void) { gd->bd->bi_boot_params = 0x80000100; return 0; }
int board_early_init_f(void) { return console_start(); }
int dram_init(void) { gd->ram_size = 128UL * 1024 * 1024; return 0; }
int dram_init_banksize(void)
{
	gd->dram[0].start = CFG_SYS_SDRAM_BASE;
	gd->dram[0].size = gd->ram_size;
	return 0;
}
int checkboard(void) { puts("Samsung Galaxy Y / BCM21553\n"); return 0; }
int board_late_init(void)
{
	/* The early BSS console state is recreated during relocation. */
	console_start();
	puts("U-Boot on Samsung Galaxy Y\nBCM21553 / ARM1136\n240x320 LCD console ready\nUART input; type help\n\n");
	return 0;
}
int timer_init(void)
{
	/* Vendor GPT requires TIMER_PWRON before reload/enable. Keep running
	 * independently of the hardware sleep/pedestal signals. */
	REG(0x08830104) = (1U << 24) | (1U << 28) | (1U << 22);
	REG(0x08830108) = 0xffffffff;
	REG(0x08830104) |= 1U << 31;
	return 0;
}
unsigned long get_tbclk(void) { return 32768; }
unsigned long long get_ticks(void)
{
	static u32 last;
	static unsigned long long ticks;
	u32 now = ~REG(0x0883010c);
	ticks += (u32)(now - last); last = now;
	return ticks;
}
unsigned long get_timer(unsigned long base) { return get_ticks() * 1000 / 32768 - base; }
void __udelay(unsigned long us)
{
	unsigned long long end = get_ticks() + ((unsigned long long)us * 32768 + 999999) / 1000000;
	/* A disabled board clock must not turn a short delay into a hard hang. */
	unsigned long budget = us < 100000 ? us * 128 + 1024 : 12801024;
	while (get_ticks() < end && --budget) {}
}
void reset_cpu(void) { for (;;) {} }

static int menu_key(void)
{
	static u32 previous;
	static unsigned int escape;
	u32 now = ~REG(0x088ce0a0) & 0x10101;
	u32 pressed = now & ~previous;
	int c;
	previous = now;
	/* Use the console mux so the menu also accepts USB ACM input. */
	if (tstc()) {
		c = getchar();
		if (c == 27) { escape = 1; return 0; }
		if (escape == 1 && (c == '[' || c == 'O')) { escape = 2; return 0; }
		if (escape == 2) {
			escape = 0;
			return c == 'A' ? 'w' : c == 'B' ? 's' : 0;
		}
		escape = 0;
		return c;
	}
	if (pressed & 1) return 'w';
	if (pressed & 0x100) return 's';
	if (pressed & 0x10000) return '\r';
	return 0;
}
int totoro_menu(const char *title, const char *const *items, unsigned int count)
{
	unsigned int selected = 0, old_selected, i;
	int key;
	if (!count || count > 16) return -1;
	in_menu = true;
	console_start();
	printf("%s\n\n", title);
	for (i = 0; i < count; i++)
		printf("%c %.28s\n", i ? ' ' : '>', items[i]);
	puts(count <= 14 ? "\nPress UP/DOWN to move,\nENTER to select" :
	     "\nUP/DOWN: move  ENTER: select");
	for (;;) {
		key = menu_key();
		old_selected = selected;
		if (key == 'q') { in_menu = false; puts("\n"); return -1; }
		if (key == 'w') selected = (selected + count - 1) % count;
		if (key == 's') selected = (selected + 1) % count;
		if (selected != old_selected) {
			/* Only two glyphs change; do not repaint 76,800 LCD pixels. */
			cells[2 + old_selected][0] = ' ';
			cells[2 + selected][0] = '>';
			glyph(0, 2 + old_selected, ' ');
			glyph(0, 2 + selected, '>');
			/* Send selection feedback to UART and ACM without LCD scrolling. */
			lcd_silent = true;
			printf("\n> %s\n", items[selected]);
			lcd_silent = false;
		}
		if (key == '\r' || key == '\n') {
			home_cancel_armed = false;
			in_menu = false;
			console_start();
			return selected;
		}
		udelay(10000);
	}
}

void totoro_usb_release_console(void)
{
	/* Dropping the last ACM stdio user unregisters that USB gadget. */
	env_set("stdin", "serial");
	env_set("stdout", "serial");
	env_set("stderr", "serial");
}

static int do_totoro(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	static const char *const items[] = {
		"Fastboot", "MMC Boot", "SD Card Boot", "MMC - Mass Storage",
		"SD - Mass Storage", "U-Boot console", "USB Console"
	};
	int selected, key;
	struct stdio_dev *acm;
	for (;;) {
		selected = totoro_menu("*** U-Boot Boot Menu ***", items, ARRAY_SIZE(items));
		switch (selected) {
		case -1: return 0;
		case 5: totoro_usb_release_console(); return 0;
		case 0:
			totoro_usb_release_console();
			puts("Fastboot: connect USB\nDownload to RAM: 0x82000000\nHome: return\n");
			run_command("fastboot usb 0", 0);
			break;
		case 1: totoro_phone_menu(true); break;
		case 2: totoro_sd_menu(); break;
		case 3: totoro_phone_menu(false); break;
		case 4:
			if (run_command("sdinit", 0)) break;
			totoro_usb_release_console();
			puts("SD USB disk: connect PC\nEject on PC, then press Home\n");
			run_command("ums 0 mmc 0", 0);
			break;
		case 6:
			puts("USB CDC ACM: connect terminal\nHome cancels waiting\n");
			/* Console mux can succeed with only serial after ACM fails. */
			run_command("setenv stdout serial,usbacm", 0);
			acm = stdio_get_by_name("usbacm");
			if (acm && acm->priv &&
			    !run_command("setenv stderr serial,usbacm", 0) &&
			    !run_command("setenv stdin serial,usbacm", 0)) return 0;
			run_command("setenv stdin serial; setenv stdout serial; setenv stderr serial", 0);
			run_command("usbdiag", 0);
			break;
		}
		in_menu = true;
		puts("Home/Enter: return to menu\n");
		while ((key = menu_key()) != '\r' && key != '\n' && key != 'q') udelay(10000);
	}
}
U_BOOT_CMD(totoro, 1, 0, do_totoro, "Galaxy Y button menu", "");
