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
#define R_CLK		0x2c		/* u16 */
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
static uint32_t rca;	/* card address, learnt by probing */

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
			return s ? s : 0x20000000;
		}
		if (s & INT_CMD_DONE)
			break;
		if (timed_out(t0, 200))
			return 0x30000000;
	}
	if (resp)
		*resp = rd32(R_RESP0);
	wr32(R_INT, INT_CMD_DONE);
	if (!data)
		return 0;

	t0 = ticks();
	for (;;) {
		uint32_t s = rd32(R_INT);
		if (s & INT_ERR) {
			wr32(R_INT, s);
			return s;
		}
		if (s & INT_BUF_RD)
			break;
		if (timed_out(t0, 500))
			return 0x40000000;
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
	static const char *const w[] = { "userdata", "boot", "recovery", "persist", "cache", "system", 0 };

	for (int i = 0; w[i]; i++) {
		const char *a = n, *b = w[i];
		while (*a && *a == *b)
			a++, b++;
		if (!*a && !*b)
			return 1;
	}
	return 0;
}

void test_emmc(void)
{
	char nm[24];
	uint32_t st, r1 = 0;

	con_str("EMMC", "ginkgo sdhci@4744000 probe (read only)");
	con_skip();
	/* Reading these registers is the first thing that can hang if the clock is gated. */
	con_str("EMMC", "reading HC regs...");
	con_hex2("EMMC VER", "ver=", rd16(R_VER), "caps=", rd32(R_CAPS));
	con_hex2("EMMC PRES", "present=", rd32(R_PRESENT), "hostctl=", rd8(R_HOSTCTL));
	con_hex2("EMMC CLK", "clk=", rd16(R_CLK), "ctrl2=", rd16(R_CTRL2));
	con_hex2("EMMC INT", "int=", rd32(R_INT), "inten=", rd32(R_INTEN));
	con_skip();

	/* Find the RCA ABL assigned: CMD13 answers with the card state only for the right one. */
	con_str("EMMC", "CMD13 scan for RCA...");
	rca = 0;
	for (uint32_t a = 1; a <= 8; a++) {
		st = sd_cmd(13, a << 16, F_R1, 0, &r1);
		con_hex2("CMD13", "rca=", a, "err/r1=", st ? st : r1);
		if (!st && ((r1 >> 9) & 0xf) == 4) {	/* current state: tran */
			rca = a;
			break;
		}
	}
	con_hex("EMMC RCA", rca);
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
			if (want(nm))
				con_hex2(nm, "first=", first, "last=", last);
		}
	}
	con_str("EMMC", "OK: eMMC readable");
}
