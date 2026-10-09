/* PNG writer for 8-bit indexed pixels. It stores uncompressed deflate blocks,
 * one per row, so it needs no compressor and no image-sized scratch buffer. */
#include "tmpl.h"
#include <Memory.h>
static short fd;
static OSErr err;
static unsigned long crc, ca, cb;
/* Writes through fd; folds the bytes into the chunk CRC when sum is set. */
static void put(const void *p, long n, int sum)
{
	const unsigned char *b = (const unsigned char *)p;
	long c = n, k;
	if (!err) err = FSWrite(fd, &c, p);
	for (; sum && n--; b++)
		for (crc ^= *b, k = 0; k < 8; k++)
			crc = (crc & 1) ? 0xEDB88320UL ^ (crc >> 1) : crc >> 1;
}
static void be(unsigned long v, int sum)
{
	unsigned char b[4];
	b[0] = (unsigned char)(v >> 24); b[1] = (unsigned char)(v >> 16);
	b[2] = (unsigned char)(v >> 8); b[3] = (unsigned char)v;
	put(b, 4, sum);
}
static void chunk(unsigned long len, const char *type)
{
	be(len, 0); crc = 0xFFFFFFFFUL; put(type, 4, 1);
}
/* 8-bit indexed image; pix points at the first row, rowBytes apart. */
OSErr png_file(const char *name, const char *pix, long rowBytes, int w, int h,
               CTabHandle colors)
{
	static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	static const unsigned char zh[2] = {0x78, 0x01}, ihdr[5] = {8, 3, 0, 0, 0};
	unsigned char *row = (unsigned char *)NewPtr(w + 6), rgb[3];
	int i, x, y;
	if (!row) return memFullErr;
	err = open_out(name, 'PNGf', &fd);
	if (err) { DisposePtr((Ptr)row); return err; }
	put(sig, 8, 0);
	chunk(13, "IHDR"); be((unsigned long)w, 1); be((unsigned long)h, 1);
	put(ihdr, 5, 1); be(~crc, 0);
	chunk(768, "PLTE");
	for (i = 0; i < 256; i++) {
		rgb[0] = rgb[1] = rgb[2] = 0;
		if (colors && i <= (*colors)->ctSize) {
			rgb[0] = (unsigned char)((*colors)->ctTable[i].rgb.red >> 8);
			rgb[1] = (unsigned char)((*colors)->ctTable[i].rgb.green >> 8);
			rgb[2] = (unsigned char)((*colors)->ctTable[i].rgb.blue >> 8);
		}
		put(rgb, 3, 1);
	}
	be(~crc, 0);
	chunk(6UL + (unsigned long)h * (unsigned long)(w + 6), "IDAT"); put(zh, 2, 1);
	ca = 1; cb = 0;
	for (y = 0; y < h; y++) {	/* one stored deflate block per row */
		row[0] = (unsigned char)(y == h - 1);
		row[1] = (unsigned char)(w + 1); row[2] = (unsigned char)((w + 1) >> 8);
		row[3] = (unsigned char)~(w + 1); row[4] = (unsigned char)(~(w + 1) >> 8);
		row[5] = 0;	/* PNG filter: none */
		BlockMoveData(pix + y * rowBytes, row + 6, w);
		put(row, w + 6, 1);
		for (x = 0; x <= w; x++) {
			ca = (ca + row[5 + x]) % 65521UL; cb = (cb + ca) % 65521UL;
		}
	}
	be((cb << 16) | ca, 1); be(~crc, 0);
	chunk(0, "IEND"); be(~crc, 0);
	FSClose(fd); DisposePtr((Ptr)row);
	return err;
}
