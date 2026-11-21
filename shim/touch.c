/*
 * ginkgo touch bring-up probe, phase 1 (read-mostly).
 *
 * Redmi Note 8: Novatek NT36672A on spi@4a88000 (QUPv3 wrapper 0, SE2), reset
 * GPIO87, IRQ GPIO88, SPI pins GPIO6-9 (function qup02). Before writing any pin
 * register we need to know which TLMM tile holds each GPIO and what the QUP clock
 * and SE firmware look like. This probe only reads, except for OR-ing the QUP0/SE2
 * clock votes into the shared GCC vote register, as the topaz driver does.
 *
 * MMU off, polled. Every risky access prints its label first, so if the phone
 * hangs the last line on screen names the culprit.
 */
#include <stdint.h>
#include "console.h"
#include "touch.h"

#define GCC		0x01400000UL
#define GCC_VOTE	0x7900cu	/* GCC_APCS_CLOCK_BRANCH_ENA_VOTE; QUP0 wrapper+SE2 = 0x13c0 */
#define GCC_RCG		0x1f3a8u	/* gcc_qupv3_wrap0_s2_clk_src CMD; CFG +4, branch CBCR -4 */
#define SE2		0x04a88000UL
#define TLMM		0x00400000UL

static inline uint32_t rd(unsigned long a) { return *(volatile uint32_t *)a; }
static inline void wr(unsigned long a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static int wait_clear(unsigned long a, uint32_t mask, unsigned ms)
{
	uint64_t t0 = ticks();

	while (rd(a) & mask)
		if (ticks() - t0 > (uint64_t)ms * 19200)
			return -1;
	return 0;
}

static void pin_row(unsigned pin)
{
	/* tile candidates for SM6125 (west/south/east); a pin lives in exactly one */
	static const unsigned long tile[3] = { 0x500000UL, 0x900000UL, 0xd00000UL };
	char l1[12] = "P   w/s", l2[12] = "P   e io";

	l1[1] = '0' + pin / 10;
	l1[2] = '0' + pin % 10;
	l2[1] = l1[1];
	l2[2] = l1[2];
	con_hex2(l1, "w=", rd(tile[0] + 0x1000UL * pin), "s=", rd(tile[1] + 0x1000UL * pin));
	con_hex2(l2, "e=", rd(tile[2] + 0x1000UL * pin), "wio=", rd(tile[0] + 0x1000UL * pin + 4));
}

void test_touch(void)
{
	con_clear(RED);
	con_str("TOUCH", "ginkgo NT36672A probe, phase 1");
	con_str("TLMM", "ctl regs per candidate tile");
	static const unsigned pins[] = { 6, 7, 8, 9, 87, 88 };

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
		pin_row(pins[i]);
	(void)TLMM;
	con_skip();

	con_str("GCC", "reading QUP vote, RCG, CBCR");
	uint32_t vote = rd(GCC + GCC_VOTE);
	con_hex2("GCC", "vote=", vote, "rcg=", rd(GCC + GCC_RCG));
	con_hex2("GCC", "cfg=", rd(GCC + GCC_RCG + 4), "cbcr=", rd(GCC + GCC_RCG - 4));

	con_str("GCC", "enabling QUP0/SE2 vote 0x13c0, XO source");
	wr(GCC + GCC_VOTE, vote | 0x13c0u);
	wr(GCC + GCC_RCG + 4, 1);		/* src = XO (parent index 0), div 1 -> 19.2 MHz */
	wr(GCC + GCC_RCG, rd(GCC + GCC_RCG) | 1u);	/* UPDATE */
	int u = wait_clear(GCC + GCC_RCG, 1u, 50);
	int h = wait_clear(GCC + GCC_RCG - 4, 1u << 31, 50);	/* CLK_OFF */
	con_hex2("GCC", "upd=", u, "halt=", h);
	con_hex2("GCC", "vote=", rd(GCC + GCC_VOTE), "cbcr=", rd(GCC + GCC_RCG - 4));
	con_skip();

	if (u || h) {
		con_str("TOUCH", "clock did not come up: stop");
		return;
	}
	con_str("SE2", "reading GENI registers");
	uint32_t fw = rd(SE2 + 0x68), hw0 = rd(SE2 + 0xe24), hw1 = rd(SE2 + 0xe28);
	con_hex2("SE2", "fw=", fw, "proto=", (fw >> 8) & 0xff);
	con_hex2("SE2", "hw0=", hw0, "hw1=", hw1);
	con_hex2("SE2", "status=", rd(SE2 + 0x40), "ifdis=", rd(SE2 + 0x64));
	con_hex2("SE2", "hw2=", rd(SE2 + 0xe2c), "ios=", rd(SE2 + 0x908));
	con_hex2("SE2", "clksel=", rd(SE2 + 0x7c), "mclk=", rd(SE2 + 0x48));
	con_hex2("SE2", "cpha=", rd(SE2 + 0x224), "cpol=", rd(SE2 + 0x230));
	con_str("TOUCH", "phase 1 done");
}
