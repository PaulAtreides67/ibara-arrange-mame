/* make_arrange_rom: build the ibara ARRANGE MAME ROM set from the player's own files.
 *
 * Players run it with their stock MAME ibara.zip (Cave's arcade set, '06. 3. 7 VER.) and their own Japanese PS2 disc
 * image of Ibara (SLPM-66301). It reads the three ARRANGE pictures from the disc (the ending/banner/bullets, the TAITO
 * logo, and the HUD atlas), packs them the same way the original tools do, applies the bundled patch to the stock ROM
 * bytes, and writes roms\ibara.zip (MAME's ibara set; start it with ibara_arrange.bat).
 *
 * This program holds nothing of the arcade ROM or of the disc: only our patch bytes and the picture cut lists. Its
 * disc-reading code (the ISO 9660 reader, the TIM2 decoder, the picture helpers and the ARRANGE/TAITO cut) is copied
 * from ibara360's tools/ps2extract/ps2extract.c, unchanged in behaviour; the HUD LZ is a C port of
 * the ARRANGE build's mkart.py (the same greedy choice).
 *
 * Build (mingw-w64 in Docker image ps2x-mingw:1, zlib 1.2.11 sources mounted at /z):
 *   MSYS_NO_PATHCONV=1 docker run --rm --memory=1g -v "<this repository>:/p" -v "<zlib 1.2.11 source folder>:/z" \
 *     ps2x-mingw:1 sh /p/tools/build.sh
 * ps2x-mingw:1 = the Dockerfile of ibara360's tools/ps2extract
 * (github.com/PaulAtreides67/ibara360): docker build -t ps2x-mingw:1 <ibara360>/tools/ps2extract
 * or: tools/build.sh [zlib-folder] (default /z). Output: tools/bin/make_arrange_rom.exe. */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <commdlg.h>
#define MKDIR(p) _mkdir(p)
#define FSEEK _fseeki64
#define STAT _stat64
#define STAT_T struct _stat64
#define PATHSEP '\\'
#else
#include <dirent.h>
#define MKDIR(p) mkdir(p, 0777)
#define FSEEK fseeko
#define STAT stat
#define STAT_T struct stat
#define PATHSEP '/'
#endif

#include "zlib.h"

#include "arrpatch.h"

/* ---- messages ------------------------------------------------------------------------------------------------------ */

static int pause_at_end = 1;

static void say(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static void wait_key(void)
{
	if (pause_at_end) {
		say("\nPress Enter to close this window.");
		getchar();
	}
}

static void die(const char *fmt, ...)
{
	va_list ap;

	printf("\nERROR: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	pause_at_end = 1;
	wait_key();
	exit(1);
}

static void *xmalloc(size_t n)
{
	void *p = calloc(1, n ? n : 1);
	if (!p)
		die("out of memory (%lu bytes).", (unsigned long)n);
	return p;
}

static uint32_t rd32le(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t rd32be(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t rd16le(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static void wr32be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void wr16be(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void wr32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void wr16le(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

/* ---- the disc: an ISO 9660 image (2048-byte sectors, or 2352-byte raw ones) or a folder of its files -------------- */

struct dfile {
	const char *name;
	uint32_t size, crc;   /* the user's copy ibara360 was made from */
	int found;
	uint64_t lba, bytes;  /* ISO: its extent; a folder: bytes only */
	char path[1024];      /* a folder: its path */
};
enum { F_PDM, F_TBL, F_N };
static struct dfile dfiles[F_N] = {
	{"IBARAR.PDM", 241377288u, 0xF0A452FEu},
	{"IBARAR.TBL", 18100u, 0xA286422Cu},
};

static FILE *iso;
static int is_dir;
static unsigned sec_size = 2048, sec_off; /* the image's sector size and where its 2048 user bytes start */

static int read_sector(uint64_t lba, uint8_t *b)
{
	if (FSEEK(iso, (int64_t)(lba * sec_size + sec_off), SEEK_SET))
		return -1;
	return fread(b, 1, 2048, iso) == 2048 ? 0 : -1;
}

/* n bytes of disc file f from its byte off */
static void dread(int f, uint64_t off, void *dst, size_t n)
{
	struct dfile *d = &dfiles[f];
	uint8_t *o = dst;

	if (off + n > d->bytes)
		die("%s: a read past its end (offset %llu, %lu bytes).", d->name, (unsigned long long)off, (unsigned long)n);
	if (is_dir) {
		FILE *fp = fopen(d->path, "rb");
		if (!fp || FSEEK(fp, (int64_t)off, SEEK_SET) || fread(o, 1, n, fp) != n)
			die("could not read %s.", d->path);
		fclose(fp);
		return;
	}
	if (sec_size == 2048) {
		if (FSEEK(iso, (int64_t)(d->lba * 2048 + off), SEEK_SET) || fread(o, 1, n, iso) != n)
			die("could not read %s from the disc image (is the ISO complete?).", d->name);
		return;
	}
	while (n) {
		uint8_t s[2048];
		const uint64_t lba = d->lba + off / 2048;
		const size_t k = (size_t)(off % 2048), m = n < 2048 - k ? n : 2048 - k;
		if (read_sector(lba, s))
			die("could not read %s from the disc image (is the image complete?).", d->name);
		memcpy(o, s + k, m);
		o += m;
		off += m;
		n -= m;
	}
}

static uint8_t *dread_all(int f)
{
	uint8_t *b = xmalloc((size_t)dfiles[f].bytes);
	dread(f, 0, b, (size_t)dfiles[f].bytes);
	return b;
}

static int name_is(const uint8_t *id, int n, const char *want)
{
	int i, w = (int)strlen(want);

	for (i = 0; i < n && id[i] != ';'; i++)
		;
	if (i > 0 && id[i - 1] == '.') /* "NAME." with no extension */
		i--;
	if (i != w)
		return 0;
	for (i = 0; i < w; i++)
		if (toupper(id[i]) != toupper((unsigned char)want[i]))
			return 0;
	return 1;
}

static void open_iso(const char *path)
{
	static const unsigned sizes[3][2] = {{2048, 0}, {2352, 16}, {2352, 24}};
	uint8_t s[2048];
	uint32_t root_lba, root_len, pos;
	int k;

	iso = fopen(path, "rb");
	if (!iso)
		die("could not open \"%s\".", path);
	for (k = 0; k < 3; k++) {
		sec_size = sizes[k][0];
		sec_off = sizes[k][1];
		if (!read_sector(16, s) && s[0] == 1 && !memcmp(s + 1, "CD001", 5))
			break;
	}
	if (k == 3)
		die("\"%s\" is not a disc image (no ISO 9660 file system in it).\n"
		    "Use an ISO made from the Ibara PS2 disc (SLPM-66301), e.g. with ImgBurn's \"Create image file from disc\".",
		    path);
	say("  %s image, %u-byte sectors, volume \"%.32s\"\n", k ? "Raw" : "ISO 9660", sec_size, (const char *)s + 40);
	root_lba = rd32le(s + 156 + 2);
	root_len = rd32le(s + 156 + 10);
	for (pos = 0; pos < root_len; pos += 2048) {
		uint8_t d[2048];
		unsigned i = 0;
		if (read_sector(root_lba + pos / 2048, d))
			die("could not read the disc image's root folder.");
		while (i < 2048 && d[i]) {
			const unsigned len = d[i], nl = d[i + 32];
			int f;
			if (len < 34 || i + len > 2048)
				break;
			for (f = 0; f < F_N; f++)
				if (!dfiles[f].found && !(d[i + 25] & 2) && name_is(d + i + 33, (int)nl, dfiles[f].name)) {
					dfiles[f].found = 1;
					dfiles[f].lba = rd32le(d + i + 2);
					dfiles[f].bytes = rd32le(d + i + 10);
				}
			i += len;
		}
	}
}

static void open_dir(const char *path)
{
	int f;

	is_dir = 1;
	for (f = 0; f < F_N; f++) {
		STAT_T st;
		snprintf(dfiles[f].path, sizeof(dfiles[f].path), "%s%c%s", path, PATHSEP, dfiles[f].name);
		if (!STAT(dfiles[f].path, &st)) {
			dfiles[f].found = 1;
			dfiles[f].bytes = (uint64_t)st.st_size;
		}
	}
}

static uint32_t file_crc(int f)
{
	const size_t chunk = 1u << 22;
	uint8_t *b = xmalloc(chunk);
	uint64_t at = 0;
	uint32_t crc = 0;
	unsigned last = 101;

	while (at < dfiles[f].bytes) {
		const size_t n = dfiles[f].bytes - at < chunk ? (size_t)(dfiles[f].bytes - at) : chunk;
		const unsigned pc = (unsigned)(at * 100 / dfiles[f].bytes);
		dread(f, at, b, n);
		crc = (uint32_t)crc32(crc, b, (uInt)n);
		at += n;
		if (f == F_PDM && pc / 10 != last / 10) {
			say("\r  checking %s ... %u%%", dfiles[f].name, pc);
			last = pc;
		}
	}
	free(b);
	return crc;
}

static void check_disc(void)
{
	int f, bad = 0;

	for (f = 0; f < F_N; f++)
		if (!dfiles[f].found)
			die("%s is not on this disc. This tool needs the Japanese Ibara PS2 disc (SLPM-66301).", dfiles[f].name);
	for (f = 0; f < F_N; f++) {
		const uint32_t crc = file_crc(f);
		const int ok = dfiles[f].bytes == dfiles[f].size && crc == dfiles[f].crc;
		say("\r  %-13s %10llu bytes  CRC32 %08X  %s\n", dfiles[f].name, (unsigned long long)dfiles[f].bytes,
		    (unsigned)crc, ok ? "matches" : "DIFFERS");
		bad += !ok;
	}
	if (bad)
		die("this disc's files are not the ones this tool knows (Ibara, SLPM-66301, VER 1.02: the\n"
		    "files above). The pictures are cut at fixed places, so a different version would give wrong ones.\n"
		    "If the image may be damaged, make it again from the disc.");
}

/* ---- TIM2 pictures (tools/card/ps2art.py's tim2_at) ----------------------------------------------------------------- */

struct pic {
	int w, h;
	uint8_t *px; /* R G B A */
};

static uint32_t pdm_off[1130], pdm_size[1130];

static void colors(const uint8_t *raw, uint32_t bytes, int ctype, uint8_t *out, uint32_t *count)
{
	uint32_t i, n;

	if (ctype == 3) {
		n = bytes / 4;
		for (i = 0; i < n; i++) {
			const int a = raw[4 * i + 3] * 255 / 128;
			out[4 * i] = raw[4 * i];
			out[4 * i + 1] = raw[4 * i + 1];
			out[4 * i + 2] = raw[4 * i + 2];
			out[4 * i + 3] = (uint8_t)(a > 255 ? 255 : a);
		}
	} else if (ctype == 2) {
		n = bytes / 3;
		for (i = 0; i < n; i++) {
			out[4 * i] = raw[3 * i];
			out[4 * i + 1] = raw[3 * i + 1];
			out[4 * i + 2] = raw[3 * i + 2];
			out[4 * i + 3] = 255;
		}
	} else {
		n = bytes / 2;
		for (i = 0; i < n; i++) {
			const unsigned v = rd16le(raw + 2 * i);
			out[4 * i] = (uint8_t)((v & 31) << 3);
			out[4 * i + 1] = (uint8_t)(((v >> 5) & 31) << 3);
			out[4 * i + 2] = (uint8_t)(((v >> 10) & 31) << 3);
			out[4 * i + 3] = (v & 0x8000) ? 255 : 0;
		}
	}
	*count = n;
}

static struct pic tim2(uint32_t off)
{
	uint8_t h[128], hd[48];
	struct pic p;
	uint32_t clutsz, imgsz, hsz, base, ncl = 0, npx, i;
	int ctype, itype;
	uint8_t *img, *clut, *cl = NULL;

	dread(F_PDM, off, h, 16);
	if (memcmp(h, "TIM2", 4))
		die("no TIM2 picture at 0x%X of IBARAR.PDM (not the disc this tool knows).", (unsigned)off);
	base = off + (h[5] ? 128 : 16);
	dread(F_PDM, base, hd, 24);
	clutsz = rd32le(hd + 4);
	imgsz = rd32le(hd + 8);
	hsz = rd16le(hd + 12);
	ctype = hd[18];
	itype = hd[19];
	p.w = rd16le(hd + 20);
	p.h = rd16le(hd + 22);
	img = xmalloc(imgsz);
	clut = xmalloc(clutsz);
	dread(F_PDM, base + hsz, img, imgsz);
	dread(F_PDM, base + hsz + imgsz, clut, clutsz);
	npx = (uint32_t)p.w * p.h;
	p.px = xmalloc((size_t)npx * 4);
	if (itype >= 1 && itype <= 3) {
		uint8_t *all = xmalloc((size_t)imgsz * 2 + 16);
		colors(img, imgsz, itype, all, &ncl);
		if (ncl < npx)
			die("TIM2 at 0x%X: short picture.", (unsigned)off);
		memcpy(p.px, all, (size_t)npx * 4);
		free(all);
	} else {
		cl = xmalloc((size_t)clutsz * 2 + 16);
		colors(clut, clutsz, ctype & 0x1F, cl, &ncl);
		if (!ncl)
			die("TIM2 at 0x%X: no colours.", (unsigned)off);
		if (itype == 5 && !(ctype & 0x80) && ncl >= 256) { /* CSM1: entries 8-15 and 16-23 of every 32 swapped */
			uint8_t t[32];
			ncl = 256;
			for (i = 0; i < 256; i += 32) {
				memcpy(t, cl + (i + 8) * 4, 32);
				memcpy(cl + (i + 8) * 4, cl + (i + 16) * 4, 32);
				memcpy(cl + (i + 16) * 4, t, 32);
			}
		}
		for (i = 0; i < npx; i++) {
			uint32_t idx;
			if (itype == 5)
				idx = i < imgsz ? img[i] : 0;
			else
				idx = i / 2 < imgsz ? (i & 1 ? img[i / 2] >> 4 : img[i / 2] & 15) : 0;
			if (idx > ncl - 1)
				idx = ncl - 1;
			memcpy(p.px + 4 * i, cl + 4 * idx, 4);
		}
		free(cl);
	}
	free(img);
	free(clut);
	return p;
}

/* src's pixel at x, y (clear outside it, as PIL's crop) */
static const uint8_t *px_at(const struct pic *s, int x, int y)
{
	static const uint8_t clear[4];
	return x < 0 || y < 0 || x >= s->w || y >= s->h ? clear : s->px + ((size_t)y * s->w + x) * 4;
}

static void pic_free(struct pic *p)
{
	free(p->px);
	p->px = NULL;
	p->w = p->h = 0;
}

/* ---- ARRANGE picture helpers (mkgfx.py: conv944.py, conv876.py, bullets/gen.py) ------------------------------------ */

/* PDM page n (256 x 256) turned into the arcade's orientation (numpy.rot90(page, 3)): arcade (row i, column j) =
 * page (x = i, y = 255 - j) */
static const uint8_t *arc_px(const struct pic *p, int i, int j)
{
	return px_at(p, i, 255 - j);
}

static void put1555(uint8_t *o, unsigned v)
{
	wr16be(o, v);
}

/* conv944 / conv876: opaque = alpha >= 128, colour min(31, (c + 4) >> 3) */
static unsigned c5_conv(const uint8_t *s)
{
	unsigned r = (s[0] + 4u) >> 3, g = (s[1] + 4u) >> 3, b = (s[2] + 4u) >> 3;
	r = r > 31 ? 31 : r;
	g = g > 31 ? 31 : g;
	b = b > 31 ? 31 : b;
	return s[3] >= 128 ? 0x8000u | r << 10 | g << 5 | b : 0;
}

/* bullets/gen.py: opaque = alpha > 0, colour min(31, rint(c / 8)) (numpy's rint: halves to even) */
static unsigned rint8(unsigned c)
{
	unsigned q = c >> 3, r = c & 7;
	if (r > 4 || (r == 4 && (q & 1)))
		q++;
	return q > 31 ? 31 : q;
}

static unsigned c5_bullet(const uint8_t *s)
{
	return s[3] > 0 ? 0x8000u | rint8(s[0]) << 10 | rint8(s[1]) << 5 | rint8(s[2]) : 0;
}

static struct pic page256(int file)
{
	struct pic p = tim2(pdm_off[file]);
	if (p.w != 256 || p.h != 256)
		die("PDM file %d is not a 256 x 256 page.", file);
	return p;
}

/* ---- the three PS2 pieces ------------------------------------------------------------------------------------------ */

static uint8_t *make_arrange_gfx(uint32_t *out_n)
{
	const uint32_t page = 0x20000, raw_n = page + 256 * 12 * 2 + 12 * page;
	uint8_t *raw = xmalloc(raw_n), *o;
	struct pic p;
	int i, j, k;

	say("  ARRANGE's pictures ...\n");
	p = page256(944); /* the ending: rows 0-239, columns 32-255 drawn, the rest clear */
	for (i = 0; i < 256; i++)
		for (j = 0; j < 256; j++)
			put1555(raw + (i * 256 + j) * 2, i < 240 && j >= 32 ? c5_conv(arc_px(&p, i, j)) : 0);
	pic_free(&p);
	p = page256(876); /* the ranking banner's label: columns 0xA0-0xAB */
	o = raw + page;
	for (i = 0; i < 256; i++)
		for (j = 0; j < 12; j++)
			put1555(o + (i * 12 + j) * 2, c5_conv(arc_px(&p, i, 0xA0 + j)));
	pic_free(&p);
	o += 256 * 12 * 2;
	for (k = 0; k < 12; k++) { /* BULLET DESIGN: COLOR 920-925, COLORLESS 938-943 */
		p = page256(k < 6 ? 920 + k : 938 + k - 6);
		for (i = 0; i < 256; i++)
			for (j = 0; j < 256; j++)
				put1555(o + (i * 256 + j) * 2, c5_bullet(arc_px(&p, i, j)));
		pic_free(&p);
		o += page;
	}
	*out_n = raw_n;
	return raw;
}

/* ARRANGE's TAITO logo (tools/arrange/mklogo.py): PDM 901 in the arcade's orientation, the 224 x 224 record at
 * u 0x20 cropped to its ink (min(R, G, B) < 248), the width made even; ARGB1555 (colour round(c / 8), halves to even)
 * through a sorted palette, run-length coded. Big-endian: u16 LOGO_W, LOGO_H, s16 LOGO_DX, LOGO_DY, u16 LOGO_NPAL,
 * u16 0, u32 LOGO_RLE_N, then LOGO_NPAL u16 palette entries (bit 15 set), then the runs (u8 count 1-255, u8 palette
 * index), mklogo.py's taito_logo.h in bytes. */
static uint8_t *make_taito_logo(uint32_t *out_n)
{
	enum { RU = 0x20, RW = 224, RD = -112 };
	struct pic p = page256(901);
	uint16_t *px, pal[256];
	uint8_t *out, *q;
	int u, v, u0 = RW, u1 = -1, v0 = RW, v1 = -1, w, h, npal = 0, i;
	size_t n, k, nr = 0;

	for (v = 0; v < RW; v++)
		for (u = 0; u < RW; u++) {
			const uint8_t *s = px_at(&p, v, 255 - (RU + u));
			int m = s[0] < s[1] ? s[0] : s[1];
			m = s[2] < m ? s[2] : m;
			if (!s[3])
				die("the TAITO logo has a hole (not the disc this tool knows).");
			if (m < 248) {
				u0 = u < u0 ? u : u0;
				u1 = u > u1 ? u : u1;
				v0 = v < v0 ? v : v0;
				v1 = v > v1 ? v : v1;
			}
		}
	u1++;
	v1++;
	if ((u1 - u0) & 1) {
		if (u1 < RW)
			u1++;
		else
			u0--;
	}
	w = u1 - u0;
	h = v1 - v0;
	n = (size_t)w * h;
	px = xmalloc(n * 2);
	for (v = 0; v < h; v++)
		for (u = 0; u < w; u++) {
			const uint8_t *s = px_at(&p, v0 + v, 255 - (RU + u0 + u));
			const uint16_t c = (uint16_t)(0x8000u | rint8(s[0]) << 10 | rint8(s[1]) << 5 | rint8(s[2]));
			px[(size_t)v * w + u] = c;
			for (i = 0; i < npal && pal[i] != c; i++)
				;
			if (i == npal) {
				if (npal == 256)
					die("the TAITO logo has more than 256 colours.");
				pal[npal++] = c;
			}
		}
	pic_free(&p);
	for (i = 1; i < npal; i++) { /* sorted, as mklogo.py */
		const uint16_t c = pal[i];
		int j = i - 1;
		while (j >= 0 && pal[j] > c) {
			pal[j + 1] = pal[j];
			j--;
		}
		pal[j + 1] = c;
	}
	out = xmalloc(16 + 2 * (size_t)npal + 2 * n);
	q = out + 16 + 2 * npal;
	for (k = 0; k < n;) {
		size_t r = 1;
		while (k + r < n && px[k + r] == px[k] && r < 255)
			r++;
		for (i = 0; pal[i] != px[k]; i++)
			;
		q[nr++] = (uint8_t)r;
		q[nr++] = (uint8_t)i;
		k += r;
	}
	wr16be(out, (unsigned)w);
	wr16be(out + 2, (unsigned)h);
	wr16be(out + 4, (unsigned)(uint16_t)(RD + u0));
	wr16be(out + 6, (unsigned)(uint16_t)(RD + v0));
	wr16be(out + 8, (unsigned)npal);
	wr16be(out + 10, 0);
	wr32be(out + 12, (uint32_t)nr);
	for (i = 0; i < npal; i++)
		wr16be(out + 16 + 2 * i, pal[i]);
	*out_n = (uint32_t)(16 + 2 * npal + nr);
	free(px);
	return out;
}

/* ---- hud_lz: C port of tools/mkart.py (same greedy choice) -------------------------------------------------------- */

struct art_piece {
	const char *name;
	int n, u, v, w, h; /* PDM page n, portrait (u, v, w, h) */
	int ax0, ay0;      /* atlas x, y */
};

static const struct art_piece ART_PIECES[] = {
	{"RANK", 887, 0, 160, 120, 12, 0, 0},
	{"FBOX", 96, 179, 244, 77, 12, 0, 120},
	{"RFILL", 887, 120, 160, 8, 8, 0, 197},
	{"BSEG", 887, 137, 160, 44, 15, 12, 0},
	{"BTOP", 887, 128, 160, 9, 15, 12, 44},
	{"BBOT", 887, 181, 160, 9, 15, 12, 53},
	{"BBACK", 887, 0, 201, 44, 6, 12, 62},
	{"BFILL", 887, 44, 201, 44, 6, 18, 62},
	{"POWN", 887, 88, 198, 34, 10, 12, 106},
	{"PNOT", 887, 122, 198, 34, 10, 12, 140},
	{"NAME0", 887, 0, 88, 34, 10, 12, 174},
	{"NAME1", 887, 34, 88, 34, 10, 12, 208},
	{"NAME2", 887, 68, 88, 34, 10, 28, 0},
	{"NAME3", 887, 102, 88, 34, 10, 28, 34},
	{"NAME4", 887, 136, 88, 34, 10, 28, 68},
	{"NAME5", 887, 170, 88, 34, 10, 28, 102},
	{"NAME6", 887, 204, 88, 34, 10, 28, 136},
};
#define ART_NPIECES (int)(sizeof(ART_PIECES) / sizeof(ART_PIECES[0]))
#define ART_W 38
#define ART_H 242

static uint32_t lz_key(const uint8_t *d, uint32_t pos)
{
	return ((uint32_t)d[pos] << 16) | ((uint32_t)d[pos + 1] << 8) | d[pos + 2];
}

/* A simple open-addressing map from 24-bit key to position list. */
struct lz_mapent {
	uint32_t key;
	uint32_t *pos;
	uint32_t n, cap;
	unsigned used;
};

struct lz_map {
	struct lz_mapent *ent;
	uint32_t nb;
};

static void lz_map_init(struct lz_map *m, uint32_t nb)
{
	uint32_t i;
	m->nb = nb;
	m->ent = xmalloc((size_t)nb * sizeof(m->ent[0]));
	for (i = 0; i < nb; i++) {
		m->ent[i].pos = NULL;
		m->ent[i].n = m->ent[i].cap = 0;
		m->ent[i].used = 0;
		m->ent[i].key = 0;
	}
}

static void lz_map_free(struct lz_map *m)
{
	uint32_t i;
	for (i = 0; i < m->nb; i++)
		free(m->ent[i].pos);
	free(m->ent);
	m->ent = NULL;
	m->nb = 0;
}

static struct lz_mapent *lz_map_get(struct lz_map *m, uint32_t key, int create)
{
	uint32_t i = (key * 2654435761u) % m->nb, start = i;
	for (;;) {
		struct lz_mapent *e = &m->ent[i];
		if (!e->used) {
			if (!create)
				return NULL;
			e->used = 1;
			e->key = key;
			e->pos = NULL;
			e->n = e->cap = 0;
			return e;
		}
		if (e->key == key)
			return e;
		i = (i + 1) % m->nb;
		if (i == start)
			die("lz_pack: hash table full");
	}
}

static void lz_map_push(struct lz_map *m, uint32_t key, uint32_t pos)
{
	struct lz_mapent *e = lz_map_get(m, key, 1);
	if (e->n == e->cap) {
		uint32_t ncap = e->cap ? e->cap * 2 : 8;
		uint32_t *np = realloc(e->pos, (size_t)ncap * sizeof(*np));
		if (!np)
			die("out of memory");
		e->pos = np;
		e->cap = ncap;
	}
	e->pos[e->n++] = pos;
}

static uint8_t *lz_pack(const uint8_t *data, uint32_t n, uint32_t *out_n)
{
	struct lz_map m;
	uint8_t *out, *fb;
	uint32_t *flags;
	uint32_t flags_n = 0, flags_cap = 0, out_len = 0, out_cap = 0, pos = 0;
	uint32_t fb_len, i;

	{
		uint32_t nb = 1;
		while (nb < (n + 4) * 2)
			nb <<= 1;
		lz_map_init(&m, nb);
	}

	flags = NULL;
	out = NULL;
	while (pos < n) {
		uint32_t best_len = 0, best_d = 0, step;
		if (pos + 3 <= n) {
			struct lz_mapent *e = lz_map_get(&m, lz_key(data, pos), 0);
			if (e) {
				uint32_t ci;
				for (ci = e->n; ci > 0; ci--) {
					uint32_t cand = e->pos[ci - 1];
					uint32_t d = pos - cand;
					uint32_t k;
					if (d > 2047)
						break;
					k = 3;
					while (k < 34 && pos + k < n && data[cand + k] == data[pos + k])
						k++;
					if (k > best_len) {
						best_len = k;
						best_d = d;
						if (k == 34)
							break;
					}
				}
			}
		}
		step = best_len >= 3 ? best_len : 1;
		if (flags_n == flags_cap) {
			uint32_t nc = flags_cap ? flags_cap * 2 : 256;
			uint32_t *nf = realloc(flags, (size_t)nc * sizeof(*nf));
			if (!nf)
				die("out of memory");
			flags = nf;
			flags_cap = nc;
		}
		if (best_len >= 3) {
			flags[flags_n++] = 1;
			if (out_len + 2 > out_cap) {
				uint32_t nc = out_cap ? out_cap * 2 : 256;
				uint8_t *no = realloc(out, nc);
				if (!no)
					die("out of memory");
				out = no;
				out_cap = nc;
			}
			wr16be(out + out_len, best_d << 5 | (best_len - 3));
			out_len += 2;
		} else {
			flags[flags_n++] = 0;
			if (out_len + 1 > out_cap) {
				uint32_t nc = out_cap ? out_cap * 2 : 256;
				uint8_t *no = realloc(out, nc);
				if (!no)
					die("out of memory");
				out = no;
				out_cap = nc;
			}
			out[out_len++] = data[pos];
		}
		{
			uint32_t q;
			for (q = pos; q < pos + step; q++) {
				if (q + 3 <= n)
					lz_map_push(&m, lz_key(data, q), q);
			}
		}
		pos += step;
	}

	fb_len = (flags_n + 7) / 8;
	fb = xmalloc(fb_len);
	memset(fb, 0, fb_len);
	for (i = 0; i < flags_n; i++)
		if (flags[i])
			fb[i >> 3] |= (uint8_t)(0x80 >> (i & 7));

	*out_n = 12 + fb_len + out_len;
	{
		uint8_t *packed = xmalloc(*out_n);
		wr32be(packed, n);
		wr32be(packed + 4, flags_n);
		wr32be(packed + 8, 12 + fb_len);
		memcpy(packed + 12, fb, fb_len);
		memcpy(packed + 12 + fb_len, out, out_len);
		free(fb);
		free(out);
		free(flags);
		lz_map_free(&m);
		return packed;
	}
}

static uint8_t *lz_unpack(const uint8_t *src, uint32_t src_n, uint32_t *out_n)
{
	uint32_t size, count, data_off, flags_off, bits = 0, i;
	uint8_t *out;
	uint32_t olen = 0;

	if (src_n < 12)
		die("lz_unpack: short header");
	size = rd32be(src);
	count = rd32be(src + 4);
	data_off = rd32be(src + 8);
	flags_off = 12;
	out = xmalloc(size ? size : 1);
	for (i = 0; i < count; i++) {
		if (i % 8 == 0) {
			if (flags_off >= src_n)
				die("lz_unpack: flag overrun");
			bits = src[flags_off++];
		}
		if (bits & 0x80) {
			uint32_t t, back, j, ln;
			if (data_off + 2 > src_n)
				die("lz_unpack: data overrun");
			t = ((uint32_t)src[data_off] << 8) | src[data_off + 1];
			data_off += 2;
			back = olen - (t >> 5);
			ln = (t & 0x1F) + 3;
			if (olen + ln > size)
				die("lz_unpack: output overrun");
			for (j = 0; j < ln; j++) {
				out[olen] = out[back];
				olen++;
				back++;
			}
		} else {
			if (data_off >= src_n || olen >= size)
				die("lz_unpack: literal overrun");
			out[olen++] = src[data_off++];
		}
		bits = (bits << 1) & 0xFF;
	}
	if (olen != size)
		die("lz_unpack: size mismatch (%u != %u)", (unsigned)olen, (unsigned)size);
	*out_n = size;
	return out;
}

static uint8_t *make_hud_lz(uint32_t *out_n)
{
	struct pic pages[1130];
	int have[1130];
	uint16_t *atlas;
	uint8_t *raw, *lz, *un;
	uint32_t raw_n, lz_n, un_n;
	int i, ay, ax;

	memset(have, 0, sizeof(have));
	atlas = xmalloc((size_t)ART_W * ART_H * sizeof(*atlas));
	for (i = 0; i < ART_W * ART_H; i++)
		atlas[i] = 0;

	for (i = 0; i < ART_NPIECES; i++) {
		const struct art_piece *p = &ART_PIECES[i];
		int n = p->n;
		if (!have[n]) {
			pages[n] = tim2(pdm_off[n]);
			have[n] = 1;
		}
		for (ay = 0; ay < p->w; ay++) {
			for (ax = 0; ax < p->h; ax++) {
				const int dst = (p->ay0 + ay) * ART_W + (p->ax0 + ax);
				const uint8_t *s = px_at(&pages[n], p->u + ay, p->v + p->h - 1 - ax);
				uint16_t v = (uint16_t)((s[3] ? 0x8000u : 0) | ((unsigned)(s[0] >> 3) << 10) |
				                        ((unsigned)(s[1] >> 3) << 5) | (unsigned)(s[2] >> 3));
				atlas[dst] = v;
			}
		}
	}
	for (i = 0; i < 1130; i++)
		if (have[i])
			pic_free(&pages[i]);

	raw_n = (uint32_t)(ART_W * ART_H * 2);
	raw = xmalloc(raw_n);
	for (i = 0; i < ART_W * ART_H; i++)
		wr16be(raw + i * 2, atlas[i]);
	free(atlas);

	lz = lz_pack(raw, raw_n, &lz_n);
	un = lz_unpack(lz, lz_n, &un_n);
	if (un_n != raw_n || memcmp(un, raw, raw_n) != 0)
		die("hud_lz: LZ round trip failed");
	free(un);
	free(raw);
	say("  ARRANGE HUD atlas: %u bytes -> %u bytes LZ\n", (unsigned)raw_n, (unsigned)lz_n);
	*out_n = lz_n;
	return lz;
}

/* ---- pieces -------------------------------------------------------------------------------------------------------- */

static uint8_t *swap16(const uint8_t *in, uint32_t n)
{
	uint8_t *o = xmalloc(n);
	uint32_t k;
	memcpy(o, in, n);
	for (k = 0; k < n / 2; k++) {
		uint8_t t = o[2 * k];
		o[2 * k] = o[2 * k + 1];
		o[2 * k + 1] = t;
	}
	return o;
}

static void build_pieces(uint8_t *pieces[AP_NPIECES], uint32_t sizes[AP_NPIECES])
{
	static const char *names[AP_NPIECES] = {"arr.gfx", "swap16(arr.gfx)", "taito_logo", "swap16(taito_logo)", "hud_lz", "swap16(hud_lz)"};
	uint32_t n;
	int i;

	pieces[0] = make_arrange_gfx(&n);
	sizes[0] = n;
	pieces[2] = make_taito_logo(&n);
	sizes[2] = n;
	pieces[4] = make_hud_lz(&n);
	sizes[4] = n;
	pieces[1] = swap16(pieces[0], sizes[0]);
	sizes[1] = sizes[0];
	pieces[3] = swap16(pieces[2], sizes[2]);
	sizes[3] = sizes[2];
	pieces[5] = swap16(pieces[4], sizes[4]);
	sizes[5] = sizes[4];

	for (i = 0; i < AP_NPIECES; i++) {
		const uint32_t crc = (uint32_t)crc32(0L, pieces[i], (uInt)sizes[i]);
		if (sizes[i] != ap_piece_size[i] || crc != ap_piece_crc[i])
			die("the disc image gave different pictures for piece %s (is it SLPM-66301?)", names[i]);
		say("  piece %s: %u bytes, CRC32 %08X  ok\n", names[i], (unsigned)sizes[i], (unsigned)crc);
	}
}

/* ---- zip reading --------------------------------------------------------------------------------------------------- */

static uint8_t *read_file_all(const char *path, uint32_t *n)
{
	FILE *f = fopen(path, "rb");
	long sz;
	uint8_t *b;
	if (!f)
		die("could not open \"%s\".", path);
	if (fseek(f, 0, SEEK_END) || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET))
		die("could not read \"%s\".", path);
	b = xmalloc((size_t)sz);
	if (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz)
		die("could not read \"%s\".", path);
	fclose(f);
	*n = (uint32_t)sz;
	return b;
}

static int ieq_name(const char *member, const char *want)
{
	/* match case-insensitively, ignore any folder part of the member name */
	const char *base = member;
	const char *p;
	for (p = member; *p; p++)
		if (*p == '/' || *p == '\\')
			base = p + 1;
	if (strlen(base) != strlen(want))
		return 0;
	for (; *base && *want; base++, want++)
		if (tolower((unsigned char)*base) != tolower((unsigned char)*want))
			return 0;
	return 1;
}

static int find_eocd(const uint8_t *z, uint32_t zn, uint32_t *cd_off, uint32_t *cd_size, uint32_t *cd_n)
{
	/* EOCD: 22 bytes minimum, comment <= 65535 */
	uint32_t i, min_off;
	if (zn < 22)
		return 0;
	min_off = zn - 22;
	if (min_off > 65535)
		min_off = zn - 22 - 65535;
	for (i = zn - 22 + 1; i-- > min_off;) {
		if (rd32le(z + i) == 0x06054b50u) {
			uint16_t comment = rd16le(z + i + 20);
			if (i + 22 + comment > zn)
				continue;
			*cd_n = rd16le(z + i + 10);
			*cd_size = rd32le(z + i + 12);
			*cd_off = rd32le(z + i + 16);
			return 1;
		}
		if (i == 0)
			break;
	}
	return 0;
}

static uint8_t *inflate_raw(const uint8_t *in, uint32_t in_n, uint32_t out_n)
{
	z_stream z;
	uint8_t *out = xmalloc(out_n ? out_n : 1);
	int rc;
	memset(&z, 0, sizeof(z));
	z.next_in = (Bytef *)in;
	z.avail_in = in_n;
	z.next_out = out;
	z.avail_out = out_n;
	if (inflateInit2(&z, -15) != Z_OK)
		die("zlib did not start for inflate.");
	rc = inflate(&z, Z_FINISH);
	if (rc != Z_STREAM_END) {
		inflateEnd(&z);
		die("could not unpack a zip member (zlib error %d).", rc);
	}
	if (z.total_out != out_n) {
		inflateEnd(&z);
		die("a zip member unpacked to %lu bytes, expected %u.", (unsigned long)z.total_out, (unsigned)out_n);
	}
	inflateEnd(&z);
	return out;
}

static void inflate_zlib(const uint8_t *in, uint32_t in_n, uint8_t *out, uint32_t out_n)
{
	z_stream z;
	int rc;

	memset(&z, 0, sizeof(z));
	z.next_in = (Bytef *)in;
	z.avail_in = in_n;
	z.next_out = out;
	z.avail_out = out_n;
	if (inflateInit(&z) != Z_OK)
		die("the bundled patch data is damaged.");
	rc = inflate(&z, Z_FINISH);
	if (rc != Z_STREAM_END || z.total_out != out_n) {
		inflateEnd(&z);
		die("the bundled patch data is damaged.");
	}
	inflateEnd(&z);
}

static uint8_t *read_member(const uint8_t *z, uint32_t zn, uint32_t local_off, uint16_t method, uint32_t comp_size, uint32_t uncomp_size)
{
	uint32_t name_len, extra_len, data_off;
	const uint8_t *data;
	if (local_off + 30 > zn || rd32le(z + local_off) != 0x04034b50u)
		die("bad local file header in the zip.");
	name_len = rd16le(z + local_off + 26);
	extra_len = rd16le(z + local_off + 28);
	data_off = local_off + 30 + name_len + extra_len;
	if (data_off + comp_size > zn)
		die("a zip member runs past the end of the file.");
	data = z + data_off;
	if (method == 0) {
		uint8_t *out = xmalloc(uncomp_size ? uncomp_size : 1);
		if (comp_size != uncomp_size)
			die("stored zip member size mismatch.");
		memcpy(out, data, comp_size);
		return out;
	}
	if (method == 8)
		return inflate_raw(data, comp_size, uncomp_size);
	die("unsupported zip compression method %u (expected stored or deflated).", (unsigned)method);
	return NULL;
}

static void load_input_zip(const char *path, uint8_t *stock[AP_NFILES])
{
	uint32_t zn, cd_off, cd_size, cd_n, i;
	uint8_t *z = read_file_all(path, &zn);
	int found[AP_NFILES];
	uint32_t pos;

	memset(found, 0, sizeof(found));
	if (!find_eocd(z, zn, &cd_off, &cd_size, &cd_n))
		die("\"%s\" is not a zip file (no end-of-central-directory record).", path);
	if ((uint64_t)cd_off + cd_size > zn)
		die("\"%s\" has a bad central directory.", path);

	pos = cd_off;
	for (i = 0; i < cd_n; i++) {
		uint16_t method, name_len, extra_len, comment_len;
		uint32_t crc, comp_size, uncomp_size, local_off;
		char name[512];
		int fi;
		if (pos + 46 > zn || rd32le(z + pos) != 0x02014b50u)
			die("\"%s\" has a bad central directory entry.", path);
		method = rd16le(z + pos + 10);
		crc = rd32le(z + pos + 16);
		comp_size = rd32le(z + pos + 20);
		uncomp_size = rd32le(z + pos + 24);
		name_len = rd16le(z + pos + 28);
		extra_len = rd16le(z + pos + 30);
		comment_len = rd16le(z + pos + 32);
		local_off = rd32le(z + pos + 42);
		if (pos + 46u + name_len + extra_len + comment_len > zn)
			die("\"%s\" has a bad central directory entry.", path);
		{
			uint32_t nl = name_len < sizeof(name) - 1 ? name_len : (uint32_t)sizeof(name) - 1;
			memcpy(name, z + pos + 46, nl);
			name[nl] = 0;
		}
		for (fi = 0; fi < AP_NFILES; fi++) {
			if (!ieq_name(name, ap_files[fi].name))
				continue;
			if (found[fi])
				die("\"%s\" contains %s more than once.", path, ap_files[fi].name);
			if (uncomp_size != ap_files[fi].size || crc != ap_files[fi].in_crc)
				die("your ibara.zip is not the set this patch is for: %s crc %08X, expected %08X (MAME's ibara set, '06. 3. 7 ver.)",
				    ap_files[fi].name, (unsigned)crc, (unsigned)ap_files[fi].in_crc);
			stock[fi] = read_member(z, zn, local_off, method, comp_size, uncomp_size);
			{
				const uint32_t got_crc = (uint32_t)crc32(0L, stock[fi], (uInt)ap_files[fi].size);
				if (got_crc != ap_files[fi].in_crc)
					die("your ibara.zip is not the set this patch is for: %s crc %08X, expected %08X (MAME's ibara set, '06. 3. 7 ver.)",
					    ap_files[fi].name, (unsigned)got_crc, (unsigned)ap_files[fi].in_crc);
			}
			found[fi] = 1;
			say("  %s: %u bytes, CRC32 %08X  ok\n", ap_files[fi].name, (unsigned)ap_files[fi].size, (unsigned)ap_files[fi].in_crc);
		}
		pos += 46u + name_len + extra_len + comment_len;
	}

	for (i = 0; i < AP_NFILES; i++)
		if (!found[i])
			die("\"%s\" is missing %s (it is not MAME's ibara set, '06. 3. 7 ver.).", path, ap_files[i].name);
	free(z);
}

/* ---- patch + output zip -------------------------------------------------------------------------------------------- */

static uint8_t *deflate_raw_buf(const uint8_t *in, uint32_t in_n, uint32_t *out_n)
{
	z_stream z;
	uLong cap;
	uint8_t *out;
	int rc;

	memset(&z, 0, sizeof(z));
	if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
		die("zlib did not start for deflate.");
	cap = deflateBound(&z, (uLong)in_n);
	out = xmalloc((size_t)cap);
	z.next_in = (Bytef *)in;
	z.avail_in = in_n;
	z.next_out = out;
	z.avail_out = (uInt)cap;
	rc = deflate(&z, Z_FINISH);
	if (rc != Z_STREAM_END)
		die("zlib failed while compressing the output.");
	*out_n = (uint32_t)z.total_out;
	deflateEnd(&z);
	return out;
}

static void apply_patch(uint8_t *stock[AP_NFILES], uint8_t *pieces[AP_NPIECES], uint32_t piece_sizes[AP_NPIECES],
                        uint8_t *out[AP_NFILES])
{
	uint8_t *lit;
	int i;

	(void)piece_sizes;
	lit = xmalloc(AP_LIT_SIZE ? AP_LIT_SIZE : 1);
	if (AP_LIT_SIZE != 0)
		inflate_zlib(ap_litz, AP_LITZ_SIZE, lit, AP_LIT_SIZE);

	for (i = 0; i < AP_NFILES; i++) {
		uint32_t j;
		out[i] = xmalloc(ap_files[i].size);
		memcpy(out[i], stock[i], ap_files[i].size);
		for (j = 0; j < ap_files[i].op_count; j++) {
			const struct ap_op *op = &ap_ops[ap_files[i].op_first + j];
			const uint8_t *src;
			if (op->kind == 0) {
				if ((uint64_t)op->from + op->len > AP_LIT_SIZE)
					die("the bundled patch data is damaged (literal overrun).");
				src = lit + op->from;
			} else if (op->kind == 1) {
				if (op->src >= AP_NPIECES || (uint64_t)op->from + op->len > piece_sizes[op->src])
					die("the bundled patch data is damaged (piece overrun).");
				src = pieces[op->src] + op->from;
			} else {
				die("the bundled patch data is damaged (bad op kind).");
			}
			if ((uint64_t)op->at + op->len > ap_files[i].size)
				die("the bundled patch data is damaged (output overrun).");
			memcpy(out[i] + op->at, src, op->len);
		}
		{
			const uint32_t crc = (uint32_t)crc32(0L, out[i], (uInt)ap_files[i].size);
			if (crc != ap_files[i].out_crc)
				die("internal error: patched %s has CRC32 %08X, expected %08X.", ap_files[i].name, (unsigned)crc, (unsigned)ap_files[i].out_crc);
		}
		say("  patched %s -> CRC32 %08X  ok\n", ap_files[i].name, (unsigned)ap_files[i].out_crc);
	}
	free(lit);
}

static void write_output_zip(const char *out_path, uint8_t *out[AP_NFILES])
{
	char tmp[1100];
	FILE *f;
	uint8_t *comp[AP_NFILES];
	uint32_t comp_n[AP_NFILES];
	uint32_t off[AP_NFILES];
	uint32_t cd_size = 0, cd_off, i;
	const uint16_t dos_time = 0;
	const uint16_t dos_date = 0x3457; /* 2006-02-23 */

	snprintf(tmp, sizeof(tmp), "%s.tmp", out_path);
	f = fopen(tmp, "wb");
	if (!f)
		die("could not write \"%s\" (is the folder writable? is the file open in another program?).", tmp);

	for (i = 0; i < AP_NFILES; i++) {
		comp[i] = deflate_raw_buf(out[i], ap_files[i].size, &comp_n[i]);
		off[i] = (uint32_t)ftell(f);
		{
			uint8_t lh[30];
			const uint16_t nl = (uint16_t)strlen(ap_files[i].name);
			wr32le(lh, 0x04034b50u);
			wr16le(lh + 4, 20);
			wr16le(lh + 6, 0);
			wr16le(lh + 8, 8);
			wr16le(lh + 10, dos_time);
			wr16le(lh + 12, dos_date);
			wr32le(lh + 14, ap_files[i].out_crc);
			wr32le(lh + 18, comp_n[i]);
			wr32le(lh + 22, ap_files[i].size);
			wr16le(lh + 26, nl);
			wr16le(lh + 28, 0);
			if (fwrite(lh, 1, 30, f) != 30 || fwrite(ap_files[i].name, 1, nl, f) != nl ||
			    fwrite(comp[i], 1, comp_n[i], f) != comp_n[i])
				die("could not write \"%s\" (is the disc full?).", tmp);
		}
	}

	cd_off = (uint32_t)ftell(f);
	for (i = 0; i < AP_NFILES; i++) {
		uint8_t ch[46];
		const uint16_t nl = (uint16_t)strlen(ap_files[i].name);
		wr32le(ch, 0x02014b50u);
		wr16le(ch + 4, 20);
		wr16le(ch + 6, 20);
		wr16le(ch + 8, 0);
		wr16le(ch + 10, 8);
		wr16le(ch + 12, dos_time);
		wr16le(ch + 14, dos_date);
		wr32le(ch + 16, ap_files[i].out_crc);
		wr32le(ch + 20, comp_n[i]);
		wr32le(ch + 24, ap_files[i].size);
		wr16le(ch + 28, nl);
		wr16le(ch + 30, 0);
		wr16le(ch + 32, 0);
		wr16le(ch + 34, 0);
		wr16le(ch + 36, 0);
		wr32le(ch + 38, 0);
		wr32le(ch + 42, off[i]);
		if (fwrite(ch, 1, 46, f) != 46 || fwrite(ap_files[i].name, 1, nl, f) != nl)
			die("could not write \"%s\" (is the disc full?).", tmp);
		cd_size += 46u + nl;
	}
	{
		uint8_t eocd[22];
		wr32le(eocd, 0x06054b50u);
		wr16le(eocd + 4, 0);
		wr16le(eocd + 6, 0);
		wr16le(eocd + 8, (unsigned)AP_NFILES);
		wr16le(eocd + 10, (unsigned)AP_NFILES);
		wr32le(eocd + 12, cd_size);
		wr32le(eocd + 16, cd_off);
		wr16le(eocd + 20, 0);
		if (fwrite(eocd, 1, 22, f) != 22)
			die("could not write \"%s\" (is the disc full?).", tmp);
	}
	if (fclose(f) != 0)
		die("could not finish \"%s\" (is the disc full?).", tmp);
	remove(out_path);
	if (rename(tmp, out_path) != 0)
		die("could not move \"%s\" into place as \"%s\".", tmp, out_path);
	for (i = 0; i < AP_NFILES; i++)
		free(comp[i]);
}

/* ---- disc index + file picking ------------------------------------------------------------------------------------- */

static void index_pdm(void)
{
	uint8_t *t = dread_all(F_TBL);
	int i;

	if (memcmp(t, "TBL\x04", 4))
		die("IBARAR.TBL is not the archive's index.");
	for (i = 0; i < 1130; i++) {
		pdm_off[i] = rd32le(t + 0x350C + 4 * i);
		pdm_size[i] = rd32le(t + 0x11BC + 8 * i + 4);
	}
	free(t);
	if (pdm_off[960] != 0x3048000 || pdm_off[1060] != 0x56C2000 || pdm_off[955] != 0x2DC5800)
		die("IBARAR.TBL is not the PS2 port's index this tool knows.");
}

static int pick_file(char *out, unsigned n, const char *filter, const char *title)
{
#ifdef _WIN32
	OPENFILENAMEA ofn;

	memset(&ofn, 0, sizeof(ofn));
	out[0] = 0;
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter = filter;
	ofn.lpstrFile = out;
	ofn.nMaxFile = n;
	ofn.lpstrTitle = title;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	return GetOpenFileNameA(&ofn) ? 0 : -1;
#else
	(void)out;
	(void)n;
	(void)filter;
	(void)title;
	return -1;
#endif
}

static void get_exe_dir(char *dir, size_t n)
{
#ifdef _WIN32
	DWORD len = GetModuleFileNameA(NULL, dir, (DWORD)n);
	if (len == 0 || len >= n)
		snprintf(dir, n, ".");
	else {
		char *s = strrchr(dir, '\\');
		char *t = strrchr(dir, '/');
		if (t > s) s = t;
		if (s) *s = 0;
		else snprintf(dir, n, ".");
	}
#else
	(void)n;
	snprintf(dir, n, ".");
#endif
}

static int ends_with_icase(const char *s, const char *suf)
{
	size_t sl = strlen(s), tl = strlen(suf);
	size_t i;
	if (tl > sl)
		return 0;
	s += sl - tl;
	for (i = 0; i < tl; i++)
		if (tolower((unsigned char)s[i]) != tolower((unsigned char)suf[i]))
			return 0;
	return 1;
}

static int same_path(const char *a, const char *b)
{
#ifdef _WIN32
	char aa[MAX_PATH], bb[MAX_PATH];
	DWORD la = GetFullPathNameA(a, MAX_PATH, aa, NULL);
	DWORD lb = GetFullPathNameA(b, MAX_PATH, bb, NULL);
	if (la == 0 || la >= MAX_PATH || lb == 0 || lb >= MAX_PATH)
		return _stricmp(a, b) == 0;
	return _stricmp(aa, bb) == 0;
#else
	char aa[1024], bb[1024];
	if (!realpath(a, aa)) snprintf(aa, sizeof(aa), "%s", a);
	if (!realpath(b, bb)) snprintf(bb, sizeof(bb), "%s", b);
	return strcmp(aa, bb) == 0;
#endif
}

static void find_in_exe_dir(const char *exe_dir, char *zip, size_t zip_n, char *disc, size_t disc_n)
{
#ifdef _WIN32
	char pat[MAX_PATH];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	int iso_count = 0;
	char iso_one[MAX_PATH] = "";

	snprintf(pat, sizeof(pat), "%s\\ibara.zip", exe_dir);
	if (zip[0] == 0) {
		DWORD attr = GetFileAttributesA(pat);
		if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
			snprintf(zip, zip_n, "%s", pat);
	}
	snprintf(pat, sizeof(pat), "%s\\*.iso", exe_dir);
	h = FindFirstFileA(pat, &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				iso_count++;
				if (iso_count == 1)
					snprintf(iso_one, sizeof(iso_one), "%s\\%s", exe_dir, fd.cFileName);
			}
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	if (disc[0] == 0 && iso_count == 1)
		snprintf(disc, disc_n, "%s", iso_one);
#else
	char p[1024];
	struct stat st;
	snprintf(p, sizeof(p), "%s/ibara.zip", exe_dir);
	if (zip[0] == 0 && stat(p, &st) == 0 && S_ISREG(st.st_mode))
		snprintf(zip, zip_n, "%s", p);
	/* non-Windows: no ISO scan needed, user passes args */
	(void)disc;
	(void)disc_n;
#endif
}

static void ensure_dir(const char *path)
{
	MKDIR(path);
}

int main(int argc, char **argv)
{
	char exe_dir[1024] = ".";
	char in_zip[1024] = "";
	char in_disc[1024] = "";
	char out_zip[1024] = "";
	uint8_t *stock[AP_NFILES];
	uint8_t *out_files[AP_NFILES];
	uint8_t *pieces[AP_NPIECES];
	uint32_t piece_sizes[AP_NPIECES];
	int opened_by_click = 0;
	int i;
	STAT_T st;

#ifdef _WIN32
	{
		DWORD pids[4];
		opened_by_click = GetConsoleProcessList(pids, 4) <= 1;
	}
#endif
	pause_at_end = opened_by_click;

	get_exe_dir(exe_dir, sizeof(exe_dir));

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--no-pause")) {
			pause_at_end = 0;
		} else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
			snprintf(out_zip, sizeof(out_zip), "%s", argv[++i]);
		} else if (argv[i][0] == '-') {
			die("unknown option: %s", argv[i]);
		} else if (ends_with_icase(argv[i], ".zip")) {
			snprintf(in_zip, sizeof(in_zip), "%s", argv[i]);
		} else {
			snprintf(in_disc, sizeof(in_disc), "%s", argv[i]);
		}
	}

	find_in_exe_dir(exe_dir, in_zip, sizeof(in_zip), in_disc, sizeof(in_disc));

	if (in_zip[0] == 0) {
		say("Pick your MAME ibara.zip (the original arcade set).\n");
		if (pick_file(in_zip, sizeof(in_zip),
		              "Zip files (*.zip)\0*.zip\0All files\0*.*\0",
		              "pick your MAME ibara.zip (the original arcade set)") != 0)
			die("No ibara.zip was chosen.");
	}
	if (in_disc[0] == 0) {
		say("Pick your Ibara PS2 disc image (SLPM-66301).\n");
		if (pick_file(in_disc, sizeof(in_disc),
		              "Disc images (*.iso;*.bin;*.img)\0*.iso;*.bin;*.img\0All files\0*.*\0",
		              "pick your Ibara PS2 disc image (SLPM-66301)") != 0)
			die("No PS2 disc image was chosen.");
	}

	if (out_zip[0] == 0) {
		char roms_dir[1100];
		snprintf(roms_dir, sizeof(roms_dir), "%s%croms", exe_dir, PATHSEP);
		ensure_dir(roms_dir);
		snprintf(out_zip, sizeof(out_zip), "%s%cibara.zip", roms_dir, PATHSEP);
	} else {
		/* create parent dir if needed */
		char parent[1100];
		snprintf(parent, sizeof(parent), "%s", out_zip);
		{
			char *s = strrchr(parent, '\\');
			char *t = strrchr(parent, '/');
			if (t > s) s = t;
			if (s) { *s = 0; ensure_dir(parent); }
		}
	}

	if (same_path(in_zip, out_zip))
		die("The output zip is the same as the input ibara.zip. Choose a different output with -o.");

	say("make_arrange_rom: building the ibara ARRANGE MAME ROM set\n\n");
	say("Reading arcade set: %s\n", in_zip);
	memset(stock, 0, sizeof(stock));
	memset(out_files, 0, sizeof(out_files));
	load_input_zip(in_zip, stock);

	say("Reading PS2 disc: %s\n", in_disc);
	if (STAT(in_disc, &st))
		die("\"%s\" is not there.", in_disc);
	if (S_ISDIR(st.st_mode))
		open_dir(in_disc);
	else
		open_iso(in_disc);
	check_disc();
	index_pdm();

	say("Building ARRANGE pieces from the disc...\n");
	build_pieces(pieces, piece_sizes);

	say("Applying the ARRANGE patch...\n");
	apply_patch(stock, pieces, piece_sizes, out_files);

	say("Writing: %s\n", out_zip);
	write_output_zip(out_zip, out_files);

	say("\nDone. The ARRANGE set was written to:\n  %s\n", out_zip);
	say("Start it with ibara_arrange.bat (it uses this roms folder, its own nvram and save states).\n");

	for (i = 0; i < AP_NFILES; i++) {
		free(stock[i]);
		free(out_files[i]);
	}
	for (i = 0; i < AP_NPIECES; i++)
		free(pieces[i]);

	wait_key();
	return 0;
}
