/*
 * ginkgo milestone 1 probe: can we talk to the internal eMMC that ABL booted from?
 *
 * Redmi Note 8 (ginkgo, SM6125): sdhci@4744000, Qualcomm sdhci-msm v5 with the
 * standard SDHCI register file. Runs with the MMU off (Device memory), polled
 * PIO only: no DMA, no cache maintenance. Nothing is ever written to the card:
 * the only commands issued are CMD13 (status) and CMD17 (single block read).
 *
 * Strategy (same as the UFS probe it replaces): reuse the state ABL left, do not
 * initialise the card. Every risky step prints a label first, so if the phone
 * hangs or resets the last line on screen says where.
 *
 * Register layout: SD Host Controller Simplified Specification 4.x.
 */
#include <stdint.h>
#include "console.h"
#include "emmc.h"

#define SDHCI_BASE	0x04744000UL	/* sdhci@4744000, ginkgo live FDT (hc_mem) */

#define R_BLKSZ		0x04		/* u16 */
#define R_BLKCNT	0x06		/* u16 */
#define R_ARG		0x08
#define R_XFERMODE	0x0c		/* u16 */
#define R_CMD		0x0e		/* u16 */
#define R_RESP0		0x10
#define R_DATA		0x20
#define R_PRESENT	0x24
#define R_HOSTCTL	0x28		/* u8 */
#define R_PWR		0x29		/* u8 */
#define R_CLK		0x2c		/* u16 */
#define R_TOUT		0x2e		/* u8 */
#define R_SWRST		0x2f		/* u8 */
#define R_INT		0x30
#define R_INTEN		0x34
#define R_CTRL2		0x3e		/* u16 */
#define R_CAPS		0x40
#define R_VER		0xfe		/* u16 */

#define INT_CMD_DONE	(1u << 0)
#define INT_XFER_DONE	(1u << 1)
#define INT_BUF_RD	(1u << 5)
#define INT_ERR		(1u << 15)
#define INT_ERR_MASK	0xffff0000u

#define PS_CMD_INHIBIT	(1u << 0)
#define PS_DAT_INHIBIT	(1u << 1)

static inline uint32_t rd32(uint32_t off) { return *(volatile uint32_t *)(SDHCI_BASE + off); }
static inline uint16_t rd16(uint32_t off) { return *(volatile uint16_t *)(SDHCI_BASE + off); }
static inline uint8_t rd8(uint32_t off) { return *(volatile uint8_t *)(SDHCI_BASE + off); }
static inline void wr8(uint32_t off, uint8_t v) { *(volatile uint8_t *)(SDHCI_BASE + off) = v; }
static inline void wr32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(SDHCI_BASE + off) = v; }
static inline void wr16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(SDHCI_BASE + off) = v; }

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

/* 19.2 MHz counter */
static int timed_out(uint64_t t0, unsigned ms)
{
	return ticks() - t0 > (uint64_t)ms * 19200;
}

static uint8_t buf[512 * 8] __attribute__((aligned(16)));

/* Reset the CMD and DAT lines (not the whole host: keep Qualcomm vendor state). */
static void reset_lines(void)
{
	uint64_t t0 = ticks();

	wr8(R_SWRST, 0x06);
	while (rd8(R_SWRST) & 0x06)
		if (timed_out(t0, 100))
			break;
}

/*
 * Issue one command and wait for completion. data != 0: single-block read of
 * 512 bytes into buf by PIO. Returns 0 or a nonzero error (int status bits).
 */
static uint32_t sd_cmd(uint32_t idx, uint32_t arg, uint32_t flags, int data, uint32_t *resp)
{
	uint64_t t0 = ticks();

	while (rd32(R_PRESENT) & (PS_CMD_INHIBIT | (data ? PS_DAT_INHIBIT : 0)))
		if (timed_out(t0, 200))
			return 0x10000000;
	wr32(R_INT, 0xffffffff);
	if (data) {
		wr16(R_BLKSZ, 512);
		wr16(R_BLKCNT, 1);
		wr16(R_XFERMODE, (1 << 4) | (1 << 1));	/* read, block count enable, no DMA */
	} else {
		wr16(R_XFERMODE, 0);
	}
	wr32(R_ARG, arg);
	wr16(R_CMD, (uint16_t)(idx << 8 | flags | (data ? (1 << 5) : 0)));

	t0 = ticks();
	for (;;) {
		uint32_t s = rd32(R_INT);
		if (s & INT_ERR) {
			wr32(R_INT, s);
			reset_lines();
			return s ? s : 0x20000000;
		}
		if (s & INT_CMD_DONE)
			break;
		if (timed_out(t0, 200)) {
			reset_lines();
			return 0x30000000;
		}
	}
	if (resp)
		for (int i = 0; i < 4; i++)
			resp[i] = rd32(R_RESP0 + 4 * i);
	wr32(R_INT, INT_CMD_DONE);
	if (!data)
		return 0;

	t0 = ticks();
	for (;;) {
		uint32_t s = rd32(R_INT);
		if (s & INT_ERR) {
			wr32(R_INT, s);
			reset_lines();
			return s;
		}
		if (s & INT_BUF_RD)
			break;
		if (timed_out(t0, 500)) {
			reset_lines();
			return 0x40000000;
		}
	}
	wr32(R_INT, INT_BUF_RD);
	for (int i = 0; i < 128; i++) {
		uint32_t w = rd32(R_DATA);
		buf[i * 4 + 0] = w;
		buf[i * 4 + 1] = w >> 8;
		buf[i * 4 + 2] = w >> 16;
		buf[i * 4 + 3] = w >> 24;
	}
	t0 = ticks();
	while (!(rd32(R_INT) & INT_XFER_DONE)) {
		if (timed_out(t0, 500))
			return 0x50000000;
	}
	wr32(R_INT, INT_XFER_DONE);
	return 0;
}

/* R1 response, 48 bits with CRC and index check */
#define F_R1	((2) | (1 << 3) | (1 << 4))
#define F_R1B	((3) | (1 << 3) | (1 << 4))
#define F_R2	((1) | (1 << 3))
#define F_R3	(2)
#define F_NONE	(0)

static uint32_t read_lba(uint32_t lba)
{
	return sd_cmd(17, lba, F_R1, 1, 0);
}

static char *name_of(const uint8_t *e, char *out)
{
	int n = 0;

	for (int i = 0; i < 36 && n < 20; i++) {
		uint8_t c = e[56 + i * 2];
		if (!c)
			break;
		out[n++] = (c >= 32 && c < 127) ? c : '.';
	}
	out[n] = 0;
	return out;
}

static int want(const char *n)
{
	static const char *const w[] = { "userdata", "boot", 0 };

	for (int i = 0; w[i]; i++) {
		const char *a = n, *b = w[i];
		while (*a && *a == *b)
			a++, b++;
		if (!*a && !*b)
			return 1;
	}
	return 0;
}

static void delay_us(unsigned us)
{
	uint64_t t0 = ticks();

	while (ticks() - t0 < (uint64_t)us * 19)
		;
}

/* SD clock = base / (2 * n); caps say base 200 MHz -> n = 250 gives ~400 kHz */
static int set_clock(unsigned n)
{
	uint64_t t0 = ticks();

	wr16(R_CLK, 0);
	wr16(R_CLK, (uint16_t)(((n & 0xff) << 8) | ((n >> 8) & 3) << 6 | 1));
	while (!(rd16(R_CLK) & 2))
		if (timed_out(t0, 150))
			return -1;
	wr16(R_CLK, rd16(R_CLK) | 4);	/* SD clock enable */
	delay_us(2000);
	return 0;
}


/* ---- phase 2: measure the real SD clock, switch to 8-bit + high speed, verify ---- */

static uint32_t hash_blocks(uint32_t lba, unsigned n, uint32_t *h_out, uint64_t *us_out)
{
	uint32_t h = 0, e = 0;
	uint64_t t0 = ticks();

	for (unsigned i = 0; i < n; i++) {
		e = read_lba(lba + i);
		if (e)
			break;
		const uint32_t *w = (const uint32_t *)buf;
		for (int j = 0; j < 128; j++)
			h = ((h << 5) | (h >> 27)) ^ w[j];
	}
	*us_out = (ticks() - t0) * 10 / 192;
	*h_out = h;
	return e;
}

/* CMD6 SWITCH, write byte; waits for the card to leave busy and checks SWITCH_ERROR */
static uint32_t switch_ext(uint32_t idx, uint32_t val)
{
	uint32_t rr[4], st;
	uint64_t t0;

	st = sd_cmd(6, (3u << 24) | (idx << 16) | (val << 8) | 1, F_R1B, 0, rr);
	if (st)
		return st;
	t0 = ticks();
	while (!(rd32(R_PRESENT) & (1u << 20)))	/* DAT0 high = not busy */
		if (timed_out(t0, 500))
			return 0x60000000;
	st = sd_cmd(13, 1u << 16, F_R1, 0, rr);
	if (!st && (rr[0] & (1u << 7)))
		return 0x70000000;
	return st;
}

static void phase2(uint32_t lba)
{
	uint32_t rr[4], st, h0 = 0, h1 = 0, n = 128;
	uint64_t us;

	/* A: slow 1-bit pass: reference hash and a measurement of the real SD clock.
	 * One 512-byte block on a 1-bit bus is ~4236 clocks including command and CRC. */
	st = hash_blocks(lba, n, &h0, &us);
	uint32_t khz = us ? (uint32_t)((uint64_t)n * 4236 * 1000 / us) : 0;
	con_hex2("EMMC T1", "err=", st, "khz=", khz);
	uint32_t base = khz * 2 * 250 / 1000;	/* MHz, divisor n=250 */
	con_hex2("EMMC T1", "baseMHz=", base, "hash=", h0);
	if (st || !base)
		return;

	/* B: EXT_CSD */
	st = sd_cmd(8, 0, F_R1, 1, rr);
	if (!st) {
		uint32_t sec = buf[212] | buf[213] << 8 | buf[214] << 16 | (uint32_t)buf[215] << 24;
		con_hex2("EXTCSD", "rev=", buf[192], "sectors=", sec);
		con_hex2("EXTCSD", "dtype=", buf[196], "partcfg=", buf[179]);
		con_hex2("EXTCSD", "bus=", buf[183], "timing=", buf[185]);
	} else {
		con_hex("EXTCSD", st);
	}

	/* C: 8-bit bus (card first, then host) */
	st = switch_ext(183, 1);
	if (!st)
		wr8(R_HOSTCTL, rd8(R_HOSTCTL) | 0x20);
	uint32_t e8 = st ? st : hash_blocks(lba, n, &h1, &us);
	con_hex2("EMMC 8BIT", "err=", e8, "match=", e8 ? 0 : (h1 == h0));

	/* D: high-speed timing, target <= 50 MHz */
	if (!e8) {
		st = switch_ext(185, 1);
		if (!st) {
			wr8(R_HOSTCTL, rd8(R_HOSTCTL) | 0x04);
			uint32_t div = (base + 99) / 100;	/* SDCLK = base / (2 * div) */
			if (div < 1)
				div = 1;
			set_clock(div);
			uint32_t ehs = hash_blocks(lba, n, &h1, &us);
			con_hex2("EMMC HS", "div=", div, "err=", ehs);
			con_hex2("EMMC HS", "match=", ehs ? 0 : (h1 == h0), "MBs*10=", us ? (uint32_t)((uint64_t)n * 512 * 10 / us) : 0);
		} else {
			con_hex("EMMC HS", st);
		}
		/* restore: legacy timing, 1-bit, then idle, so ABL finds the card as it expects */
		set_clock(250);
		switch_ext(185, 0);
		wr8(R_HOSTCTL, rd8(R_HOSTCTL) & ~0x04);
		switch_ext(183, 0);
		wr8(R_HOSTCTL, rd8(R_HOSTCTL) & ~0x20);
	}
	sd_cmd(0, 0, F_NONE, 0, 0);
	con_str("EMMC", "phase 2 done; card back to idle");
}

static uint32_t boot_lba = 0xf0000;

void test_emmc(void)
{
	char nm[24];
	uint32_t st, rr[4] = { 0 };

	con_clear(RED);
	con_str("EMMC", "ginkgo sdhci@4744000 (read only)");
	con_hex2("EMMC VER", "ver=", rd16(R_VER), "caps=", rd32(R_CAPS));
	con_hex2("EMMC PRES", "present=", rd32(R_PRESENT), "hostctl=", rd8(R_HOSTCTL));
	con_hex2("EMMC CLK", "clk=", rd16(R_CLK), "pwr=", rd8(R_PWR));
	con_hex2("EMMC INT", "int=", rd32(R_INT), "inten=", rd32(R_INTEN));
	con_skip();

	/* ABL left the status enables off and the SD clock gated: bring the host up ourselves. */
	wr32(R_INTEN, 0x01ff01ff);
	wr32(R_INT, 0xffffffff);
	reset_lines();
	wr8(R_TOUT, 0x0e);
	if (!(rd8(R_PWR) & 1))
		wr8(R_PWR, 0x0b);	/* bus power on, 1.8 V (eMMC VCCQ); regulators are external */
	int cr = set_clock(250);
	con_hex2("EMMC UP", "clkrc=", cr, "clk=", rd16(R_CLK));
	con_hex2("EMMC UP", "pwr=", rd8(R_PWR), "pres=", rd32(R_PRESENT));
	con_skip();

	/* Standard eMMC identification, 1-bit legacy timing. No CMD6, nothing persistent. */
	st = sd_cmd(0, 0, F_NONE, 0, 0);
	con_hex("CMD0", st);
	delay_us(2000);
	int ready = 0;
	for (int i = 0; i < 1000 && !st; i++) {
		st = sd_cmd(1, 0x40ff8080u, F_R3, 0, rr);
		if (st || (rr[0] & 0x80000000u)) {
			ready = !st;
			break;
		}
		delay_us(1000);
	}
	con_hex2("CMD1", "err=", st, "ocr=", rr[0]);
	if (!ready)
		return;
	st = sd_cmd(2, 0, F_R2, 0, rr);
	con_hex2("CMD2", "err=", st, "cid3=", rr[3]);
	if (st)
		return;
	st = sd_cmd(3, 1u << 16, F_R1, 0, rr);
	con_hex2("CMD3", "err=", st, "r1=", rr[0]);
	if (st)
		return;
	st = sd_cmd(7, 1u << 16, F_R1B, 0, rr);
	con_hex2("CMD7", "err=", st, "r1=", rr[0]);
	if (st)
		return;
	st = sd_cmd(13, 1u << 16, F_R1, 0, rr);
	con_hex2("CMD13", "err=", st, "r1=", rr[0]);
	con_skip();

	con_str("EMMC", "CMD17 read LBA 1 (GPT header)");
	st = read_lba(1);
	con_hex("EMMC RD", st);
	if (st)
		return;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (buf[i] >= 32 && buf[i] < 127) ? buf[i] : '.';
	sig[8] = 0;
	con_str("EMMC GPT", sig);
	uint32_t nent = buf[80] | buf[81] << 8 | buf[82] << 16, esz = buf[84] | buf[85] << 8;
	uint32_t elba = buf[72] | buf[73] << 8 | buf[74] << 16;
	con_hex2("EMMC GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || nent > 256 || elba == 0)
		return;

	/* 4 entries per 512-byte sector. */
	for (uint32_t s = 0; s * 4 < nent; s++) {
		if (read_lba(elba + s))
			return;
		for (int k = 0; k < 4 && s * 4 + k < nent; k++) {
			const uint8_t *e = buf + k * 128;
			uint32_t first = e[32] | e[33] << 8 | e[34] << 16 | (uint32_t)e[35] << 24;
			uint32_t last = e[40] | e[41] << 8 | e[42] << 16 | (uint32_t)e[43] << 24;
			if (!first)
				continue;
			name_of(e, nm);
			if (want(nm)) {
				con_hex2(nm, "first=", first, "last=", last);
				if (nm[0] == 'b')
					boot_lba = first;
			}
		}
	}
	con_str("EMMC", "OK: eMMC readable");
	con_skip();
	phase2(boot_lba);
}
