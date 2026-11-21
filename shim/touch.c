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


#define WEST		0x500000UL
#define SE_STATUS	0x040
#define SE_CLK_CFG	0x048
#define SE_CLK_SEL	0x07c
#define SE_OUTCTRL	0x024
#define SE_CGC		0x028
#define SE_FORCEDEF	0x020
#define SE_DMA_MODE	0x258
#define SE_CPHA		0x224
#define SE_CPOL		0x230
#define SE_DEMUX_INV	0x24c
#define SE_DEMUX_SEL	0x250
#define SE_WORD_LEN	0x268
#define SE_TXLEN	0x26c
#define SE_RXLEN	0x270
#define SE_CMD0		0x600
#define SE_IRQ		0x610
#define SE_IRQ_EN	0x614
#define SE_IRQ_CLR	0x618
#define SE_TXFIFO	0x700
#define SE_RXFIFO	0x780
#define SE_RXSTAT	0x804
#define SE_TXWM		0x80c
#define SE_RXWM		0x810
#define SE_RXRFR	0x814

#define IRQ_DONE	(1u << 0)
#define IRQ_ERR		((7u << 1) | (3u << 24) | (3u << 28))

static void delay_ms(unsigned ms)
{
	uint64_t t0 = ticks();

	while (ticks() - t0 < (uint64_t)ms * 19200)
		;
}

static void pin_cfg(unsigned pin, unsigned mux, unsigned pull, unsigned drv, unsigned oe)
{
	wr(WEST + 0x1000UL * pin, (mux << 2) | pull | (drv << 6) | (oe ? 0x200u : 0u));
}

static void pin_out(unsigned pin, unsigned v)
{
	wr(WEST + 0x1000UL * pin + 4, v ? 2u : 0u);
}

static unsigned pin_in(unsigned pin)
{
	return rd(WEST + 0x1000UL * pin + 4) & 1u;
}

/* One SPI command (TX_ONLY=1 or RX_ONLY=2); frag keeps CS asserted afterwards. */
static int spi_cmd(unsigned op, const uint8_t *tx, uint8_t *rx, unsigned n, int frag)
{
	unsigned sent = 0, got = 0;
	uint64_t t0;

	if (!n || n > 120)
		return -1;
	t0 = ticks();
	while (rd(SE2 + SE_STATUS) & 1u)
		if (ticks() - t0 > 20 * 19200ull)
			return -2;
	wr(SE2 + SE_IRQ_CLR, 0xffffffffu);
	wr(SE2 + (op == 1 ? SE_TXLEN : SE_RXLEN), n);
	wr(SE2 + SE_CMD0, (op << 27) | (frag ? 4u : 0u));
	if (op == 1) {
		/* first fill: up to (16 - 1) FIFO words; commands here are a few bytes */
		while (sent < n && sent < 60) {
			uint32_t w = 0;
			for (unsigned j = 0; j < 4 && sent + j < n; j++)
				w |= (uint32_t)tx[sent + j] << (8 * j);
			wr(SE2 + SE_TXFIFO, w);
			sent += 4;
		}
	}
	t0 = ticks();
	for (;;) {
		uint32_t irq = rd(SE2 + SE_IRQ);

		if (irq & IRQ_ERR) {
			wr(SE2 + SE_IRQ_CLR, irq);
			return -3;
		}
		if (op == 2) {
			uint32_t st = rd(SE2 + SE_RXSTAT);
			unsigned words = st & 0x1ffffffu;
			while (words--) {
				uint32_t w = rd(SE2 + SE_RXFIFO);
				unsigned valid = 4;
				if ((st & (1u << 31)) && !words) {
					unsigned v = (st >> 28) & 7;
					if (v && v < 4)
						valid = v;
				}
				for (unsigned j = 0; j < valid && got < n; j++)
					rx[got++] = (uint8_t)(w >> (8 * j));
			}
		}
		if (irq & IRQ_DONE) {
			wr(SE2 + SE_IRQ_CLR, irq);
			if (op == 2) {	/* final drain after DONE */
				uint32_t st = rd(SE2 + SE_RXSTAT);
				unsigned words = st & 0x1ffffffu;
				while (words--) {
					uint32_t w = rd(SE2 + SE_RXFIFO);
					unsigned valid = 4;
					if ((st & (1u << 31)) && !words) {
						unsigned v = (st >> 28) & 7;
						if (v && v < 4)
							valid = v;
					}
					for (unsigned j = 0; j < valid && got < n; j++)
						rx[got++] = (uint8_t)(w >> (8 * j));
				}
			}
			return (op == 2 && got != n) ? -4 : 0;
		}
		if (ticks() - t0 > 50 * 19200ull) {
			wr(SE2 + 0x604, 4);	/* cancel */
			return -5;
		}
	}
}

static int nvt_read(uint32_t a, uint8_t *buf, unsigned n)
{
	uint8_t h[3] = { 0xff, (uint8_t)(a >> 15), (uint8_t)(a >> 7) }, r = a & 0x7f;
	int e = spi_cmd(1, h, 0, 3, 0);

	if (e)
		return e;
	e = spi_cmd(1, &r, 0, 1, 1);
	if (e)
		return e;
	return spi_cmd(2, 0, buf, n, 0);
}

static int nvt_write1(uint32_t a, uint8_t v)
{
	uint8_t h[3] = { 0xff, (uint8_t)(a >> 15), (uint8_t)(a >> 7) };
	uint8_t d[2] = { (uint8_t)((a & 0x7f) | 0x80), v };
	int e = spi_cmd(1, h, 0, 3, 0);

	return e ? e : spi_cmd(1, d, 0, 2, 0);
}

/* The IC returns one dummy byte (0xff) before the data: read n+1 and return the real byte. */
static uint8_t nvt_rd1(uint32_t a)
{
	uint8_t t[2] = { 0, 0 };

	nvt_read(a, t, 2);
	return t[1];
}

#ifdef HAVE_TSFW
extern const unsigned char nvt_fw[];
extern const unsigned nvt_fw_len;

/* TX_ONLY command whose byte stream is a[0..na) followed by b[0..nb); streams through the 16-word FIFO. */
static int spi_tx2(const uint8_t *a, unsigned na, const uint8_t *b, unsigned nb)
{
	unsigned n = na + nb, sent = 0;
	uint64_t t0 = ticks();

	if (!n || n > 0xffffff)
		return -1;
	while (rd(SE2 + SE_STATUS) & 1u)
		if (ticks() - t0 > 20 * 19200ull)
			return -2;
	wr(SE2 + SE_IRQ_CLR, 0xffffffffu);
	wr(SE2 + SE_TXLEN, n);
	wr(SE2 + SE_CMD0, 1u << 27);
	t0 = ticks();
	for (;;) {
		uint32_t irq = rd(SE2 + SE_IRQ);

		if (irq & IRQ_ERR) {
			wr(SE2 + SE_IRQ_CLR, irq);
			return -3;
		}
		while (sent < n && (rd(SE2 + 0x800) & 0xfffffffu) < 16) {
			uint32_t w = 0;
			for (unsigned j = 0; j < 4 && sent + j < n; j++) {
				unsigned i = sent + j;
				uint8_t v = i < na ? a[i] : b[i - na];
				w |= (uint32_t)v << (8 * j);
			}
			wr(SE2 + SE_TXFIFO, w);
			sent += 4;
		}
		if (irq & IRQ_DONE) {
			wr(SE2 + SE_IRQ_CLR, irq);
			return sent >= n ? 0 : -4;
		}
		if (ticks() - t0 > 400 * 19200ull) {
			wr(SE2 + 0x604, 4);
			return -5;
		}
	}
}

/* NT36xxx write of n bytes at address a (auto-increment): page header message, then reg|0x80 + data. */
static int nvt_write_buf(uint32_t a, const uint8_t *data, unsigned n)
{
	uint8_t h[3] = { 0xff, (uint8_t)(a >> 15), (uint8_t)(a >> 7) };
	uint8_t r = (uint8_t)((a & 0x7f) | 0x80);
	int e = spi_cmd(1, h, 0, 3, 0);

	return e ? e : spi_tx2(&r, 1, data, n);
}

static uint32_t le32(const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

#define EVT		0x21c00u	/* NT36672A event buffer (0x22D00 is inside the raw-data pipe) */

/* Returns 0 on success; *stage says how far it got. */
static int nvt_download_fw(unsigned *stage, uint32_t *nparts_out)
{
	const unsigned char *d = nvt_fw;
	uint32_t end = le32(d), pos = 0x30, info = 0;
	unsigned ovly = (d[0x28] & 0x10) ? (d[0x28] & 0x0f) : 0;
	struct { uint32_t bin, sram, size, crc; } t[40];
	unsigned np = 0;
	int e;

	while (pos < end) {
		info++;
		pos += 0x10;
	}
	if (2 + info + ovly > 40)
		return -10;
	for (unsigned l = 0; l < 2; l++) {
		t[np].bin = le32(d + l * 12);
		t[np].sram = le32(d + 4 + l * 12);
		t[np].size = le32(d + 8 + l * 12);
		t[np].crc = le32(d + 0x18 + l * 4);
		np++;
	}
	for (unsigned i = 0; i < info; i++) {
		unsigned o = 0x30 + 0x10 * i;
		t[np].sram = le32(d + o);
		t[np].size = le32(d + o + 4);
		t[np].bin = le32(d + o + 8);
		t[np].crc = le32(d + o + 12);
		np++;
	}
	for (unsigned i = 0; i < ovly; i++) {
		unsigned o = t[1].bin + 0x10 * i;
		t[np].sram = le32(d + o);
		t[np].size = le32(d + o + 4);
		t[np].bin = le32(d + o + 8);
		t[np].crc = le32(d + o + 12);
		np++;
	}
	*nparts_out = np;
	for (unsigned l = 0; l < np; l++)
		if (t[l].size && t[l].bin + t[l].size > nvt_fw_len)
			return -11;

	/* engine reset into idle, then bootloader reset */
	*stage = 1;
	nvt_write1(0x7fff80, 0x5a);
	delay_ms(45);
	nvt_write1(0x3f0fe, 0x69);
	delay_ms(35);
	nvt_write1(0x3f310, 0);

	/* all partitions to their SRAM addresses (size + 1 bytes, as the vendor driver does) */
	*stage = 2;
	for (unsigned l = 0; l < np; l++) {
		unsigned size = t[l].size;
		if (!size)
			continue;
		size += 1;
		for (unsigned j = 0; j < size; j += 2048) {
			unsigned len = size - j < 2048 ? size - j : 2048;
			e = nvt_write_buf(t[l].sram + j, d + t[l].bin + j, len);
			if (e)
				return e;
		}
	}

	/* ILM/DLM descriptors and checksums */
	*stage = 3;
	for (unsigned l = 0; l < 2; l++) {
		uint8_t v[4];
		static const uint32_t des[2] = { 0x3f128, 0x3f12c }, len[2] = { 0x3f118, 0x3f130 },
		    crc[2] = { 0x3f100, 0x3f104 };
		v[0] = t[l].sram; v[1] = t[l].sram >> 8; v[2] = t[l].sram >> 16;
		if ((e = nvt_write_buf(des[l], v, 3)))
			return e;
		v[0] = t[l].size; v[1] = t[l].size >> 8; v[2] = t[l].size >> 16;
		if ((e = nvt_write_buf(len[l], v, 3)))
			return e;
		v[0] = t[l].crc; v[1] = t[l].crc >> 8; v[2] = t[l].crc >> 16; v[3] = t[l].crc >> 24;
		if ((e = nvt_write_buf(crc[l], v, 4)))
			return e;
	}
	*stage = 4;
	uint8_t b1 = nvt_rd1(0x3f30e);			/* BLD_CRC_EN: read-modify-write, bit 7 only */
	b1 |= 0x80;
	nvt_write1(0x3f30e, b1);
	nvt_write1(EVT | 0x60, 0);			/* clear reset-complete */
	nvt_write1(EVT | 0x50, 0xae);			/* host cmd */
	nvt_write1(0x3f10d, 1);				/* boot ready */
	delay_ms(10);
	return 0;
}
#endif

static void dump8(const char *l, int err, const uint8_t *b)
{
	con_hex2(l, "err=", (uint32_t)err, "b0..3=",
	    b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24);
	con_hex2("", "b4..7=", b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24, "", 0);
}

struct tstat { uint32_t frames, touches, lastx, lasty, maxx, maxy, recov; };

static void __attribute__((unused)) poll_events(uint32_t base, struct tstat *t)
{
	uint8_t p[66];

	if (nvt_read(base, p, 66))
		return;
	t->frames++;
	if (p[0] == 0xfd || p[0] == 0xfe || p[0] == 0x77) {
		t->recov++;
		return;
	}
	for (unsigned i = 0; i < 10; i++) {
		const uint8_t *f = p + 6 * i + 1;
		unsigned id = f[0] >> 3, st = f[0] & 7;
		if (id && id <= 10 && (st == 1 || st == 2)) {
			uint32_t x = ((uint32_t)f[1] << 4) + (f[3] >> 4);
			uint32_t y = ((uint32_t)f[2] << 4) + (f[3] & 0xf);
			t->touches++;
			t->lastx = x;
			t->lasty = y;
			if (x > t->maxx)
				t->maxx = x;
			if (y > t->maxy)
				t->maxy = y;
		}
	}
}

void test_touch(void)
{
	uint8_t b[8];
	int e;

	con_clear(RED);
	con_str("TOUCH", "ginkgo NT36672A probe, phase 2");

	/* clocks: SE2 vote + XO source (verified in phase 1) */
	wr(GCC + GCC_VOTE, rd(GCC + GCC_VOTE) | 0x13c0u);
	wr(GCC + GCC_RCG + 4, 1);
	wr(GCC + GCC_RCG, rd(GCC + GCC_RCG) | 1u);
	int u = wait_clear(GCC + GCC_RCG, 1u, 50);
	int h = wait_clear(GCC + GCC_RCG - 4, 1u << 31, 50);
	con_hex2("CLK", "upd=", u, "halt=", h);
	if (u || h)
		return;

	/* pins: SPI on GPIO6-9 (qup02, 6 mA, no pull), IRQ GPIO88 input, reset GPIO87 kept */
	for (unsigned p = 6; p <= 9; p++)
		pin_cfg(p, 1, 0, 2, 0);
	pin_cfg(88, 0, 0, 3, 0);
	con_hex2("PIN", "p6..9=", rd(WEST + 0x6000) | rd(WEST + 0x7000) << 8 | rd(WEST + 0x8000) << 16 | rd(WEST + 0x9000) << 24,
	    "irq=", pin_in(88));

	/* GENI: init as geni_se_init + spi_geni_init, FIFO mode, SPI mode 0, 8-bit, 4.8 MHz */
	wr(SE2 + 0xe18, 0);
	wr(SE2 + 0xe1c, 0);
	wr(SE2 + 0x644, 0);
	wr(SE2 + 0x648, 0xffffffffu);
	wr(SE2 + SE_DMA_MODE, 0);
	wr(SE2 + SE_CGC, rd(SE2 + SE_CGC) | 0x7fu);
	wr(SE2 + 0xe30, rd(SE2 + 0xe30) | 0xfu);
	wr(SE2 + SE_OUTCTRL, 0x7f);
	wr(SE2 + SE_FORCEDEF, 1);
	wr(SE2 + SE_CPHA, 0);
	wr(SE2 + SE_CPOL, 0);
	wr(SE2 + SE_DEMUX_SEL, 0);
	wr(SE2 + SE_DEMUX_INV, 0);
	wr(SE2 + 0x22c, 0);
	wr(SE2 + 0x25c, rd(SE2 + 0x25c) & ~2u);
	wr(SE2 + 0x254, 0);
	wr(SE2 + 0x260, 0x0007f8feu);
	wr(SE2 + 0x264, 0x000ffefeu);
	wr(SE2 + 0x284, 0x0007f8feu);
	wr(SE2 + 0x288, 0x000ffefeu);
	wr(SE2 + SE_WORD_LEN, 4);			/* 8 bits per word */
	wr(SE2 + SE_CLK_SEL, 0);
	wr(SE2 + SE_CLK_CFG, (4u << 4) | 1u);		/* 19.2 MHz / 4 = 4.8 MHz */
	wr(SE2 + SE_TXWM, 0);
	wr(SE2 + SE_RXWM, 0);
	wr(SE2 + SE_RXRFR, 15);
	wr(SE2 + SE_IRQ_EN, IRQ_DONE | IRQ_ERR | (1u << 4) | (1u << 5) | (1u << 26) | (1u << 27) | (1u << 30));
	wr(SE2 + SE_IRQ_CLR, 0xffffffffu);
	con_hex2("SE2", "stat=", rd(SE2 + SE_STATUS), "cfg=", rd(SE2 + SE_CLK_CFG));

	/* reset pulse on GPIO87 (active low), then wait for the IC */
	pin_out(87, 0);
	delay_ms(20);
	pin_out(87, 1);
	delay_ms(150);
	con_hex2("RST", "pulse", 0, "irq=", pin_in(88));

	/* bootloader reset + chip id, a few attempts */
	uint8_t id[8] = { 0 };
	for (int i = 0; i < 5; i++) {
		nvt_write1(0x3f0fe, 0x69);
		delay_ms(35);
		nvt_write1(0x3f310, 0);
		delay_ms(10);
		e = nvt_read(0x1f64e, id, 7);
		if (!e && id[1] == 0x0a && id[4] == 0x72 && id[5] == 0x66 && id[6] == 0x03)
			break;
	}
	dump8("ID", e, id);
	con_str("ID", id[1] == 0x0a && id[4] == 0x72 ? "NT36672A recognised" : "unexpected id");
	con_skip();

	static const uint32_t ev[2] = { 0x21c00, 0x22d00 };
#ifdef HAVE_TSFW
	unsigned stage = 0;
	uint32_t nparts = 0;
	e = nvt_download_fw(&stage, &nparts);
	con_hex2("FW", "err=", (uint32_t)e, "stage=", stage);
	con_hex2("FW", "parts=", nparts, "len=", nvt_fw_len);
	uint8_t rs[6] = { 0 }, fi[16] = { 0 };
	int tries = 0, re = -1;
	for (; tries < 50; tries++) {
		re = nvt_read(EVT | 0x60, rs, 6);
		if (!re && rs[1] >= 0xa0 && rs[1] <= 0xaf)
			break;
		delay_ms(10);
	}
	con_hex2("FW", "rstst=", rs[1], "tries=", tries);
	/* diagnostics: read the chip memory back and compare with the file; HW-computed CRCs */
	{
		uint8_t c0[8] = { 0 }, c1[8] = { 0 }, cr[8] = { 0 }, bt[2] = { 0 }, st6[6] = { 0 }, ln[3] = { 0 };
		uint32_t ilm_bin = le32(nvt_fw + 0), ilm_sram = le32(nvt_fw + 4), dlm_bin = le32(nvt_fw + 12), dlm_sram = le32(nvt_fw + 16);
		int r0 = nvt_read(ilm_sram, c0, 8), r1 = nvt_read(dlm_sram, c1, 8);
		con_hex2("ILM", "chip=", le32(c0), "file=", le32(nvt_fw + ilm_bin));
		con_hex2("ILM", "chip4=", le32(c0 + 4), "file4=", le32(nvt_fw + ilm_bin + 4));
		con_hex2("DLM", "chip=", le32(c1), "file=", le32(nvt_fw + dlm_bin));
		con_hex2("RD", "ilm err=", (uint32_t)r0, "dlm err=", (uint32_t)r1);
		nvt_read(0x3f120, cr, 8);
		con_hex2("RCRC", "ilm=", le32(cr), "dlm=", le32(cr + 4));
		con_hex2("EXPC", "ilm=", le32(nvt_fw + 0x18), "dlm=", le32(nvt_fw + 0x1c));
		bt[0] = nvt_rd1(0x3f10d);
		bt[1] = nvt_rd1(0x3f30e);
		nvt_read(0x3f118, ln, 3);
		con_hex2("REG", "rdy=", bt[0], "bldcrc=", bt[1]);
		con_hex2("REG", "ilmlen=", ln[0] | ln[1] << 8 | ln[2] << 16, "exp=", le32(nvt_fw + 8));
		nvt_read(EVT | 0x60, st6, 6);
		con_hex2("ST", "b0..3=", le32(st6), "b4,5=", st6[4] | st6[5] << 8);
	}
	int fe = nvt_read(EVT | 0x78, fi, 16);
	con_hex2("FWI", "err=", (uint32_t)fe, "b0..3=", fi[0] | fi[1] << 8 | fi[2] << 16 | (uint32_t)fi[3] << 24);
	con_hex2("", "b4..7=", fi[4] | fi[5] << 8 | fi[6] << 16 | (uint32_t)fi[7] << 24, "b8..11=", fi[8] | fi[9] << 8 | fi[10] << 16 | (uint32_t)fi[11] << 24);
#else
	con_str("FW", "not built in");
#endif
	/* touch window: BLUE screen for 25 s, show decoded fingers and raw bytes of changed frames */
	con_skip();
	con_str("TOUCH", ">>> TOCA LA PANTALLA AHORA (10 s) <<<");
	uint8_t idle[66], cur[66];
	int ie = nvt_read(ev[0], idle, 66);
	con_hex2("IDLE", "err=", (uint32_t)ie, "b0..3=", idle[0] | idle[1] << 8 | idle[2] << 16 | (uint32_t)idle[3] << 24);
	struct tstat tb = { 0 };
	uint64_t t0 = ticks(), tlast = 0;
	unsigned lines = 0, changed = 0;
	while (ticks() - t0 < 10 * 19200000ull) {
		if (nvt_read(ev[0], cur, 66)) {
			continue;
		}
		tb.frames++;
		unsigned diff = 0;
		for (unsigned i = 1; i < 66; i++)
			if (cur[i] != idle[i])
				diff++;
		if (diff) {
			changed++;
			if (ticks() - tlast > 250 * 19200ull && lines < 8) {
				tlast = ticks();
				lines++;
				con_hex2("D", "n=", diff, "b0..3=", cur[0] | cur[1] << 8 | cur[2] << 16 | (uint32_t)cur[3] << 24);
				con_hex2("", "b4..7=", cur[4] | cur[5] << 8 | cur[6] << 16 | (uint32_t)cur[7] << 24,
				    "b8..11=", cur[8] | cur[9] << 8 | cur[10] << 16 | (uint32_t)cur[11] << 24);
			}
		}
		delay_ms(2);
	}
	con_str("FIN", "ventana terminada, ya no toques");
	con_hex2("END", "frames=", tb.frames, "changed=", changed);
	con_hex2("END", "irq=", pin_in(88), "lines=", lines);
	con_str("TOUCH", "phase 2 done");
}
