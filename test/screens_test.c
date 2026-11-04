/* Two screens in one frame: the layouts' geometry, the preview they compose,
 * and the stylus - its pointer, its cursor and its movement.
 *
 * Offline and instant, like check-cheevos: it links src/screens.c and
 * src/scale.c and nothing else, so it needs no core, no ROM and no display.
 * The numbers are the GKD Pixel 2's, a 640x480 surface, and DSperate's frame,
 * 256x384, because that pair is what the layouts were worked out for and the
 * one a regression would be seen on first. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "screens.h"

static int g_fail, g_pass;

static void ck(int cond, const char *what)
{
	if (cond) { g_pass++; return; }
	g_fail++;
	printf("  FAIL %s\n", what);
}

static int is(diatom_rect r, int x, int y, int w, int h)
{
	return r.x == x && r.y == y && r.w == w && r.h == h;
}

static void layouts(void)
{
	diatom_plane p[DIATOM_PLANES_MAX];
	diatom_rect b;
	int n;

	/* Stacked, by aspect: the frame as one 320x480 picture, cut at 240 -
	 * where a plain present of the whole frame put each half. */
	n = scr_planes(SCR_STACKED, false, DIATOM_SCALE_ASPECT_FIT, 256, 384, 640, 480, p);
	ck(n == 2, "stacked has two planes");
	ck(is(p[0].src, 0, 0, 256, 192) && is(p[0].dst, 160, 0, 320, 240), "stacked top at 160,0 320x240");
	ck(is(p[1].src, 0, 192, 256, 192) && is(p[1].dst, 160, 240, 320, 240), "stacked bottom under it");
	b = scr_bounds(p, n);
	ck(is(b, 160, 0, 320, 480), "stacked bounds are the plain rect");
	ck(is(b, diatom_scale_rect(DIATOM_SCALE_ASPECT_FIT, 256, 384, 256.0 / 384, 640, 480).x, 0, 320, 480),
	   "the same x the whole-frame rect has");

	/* Swapped, the bottom screen goes on top. */
	n = scr_planes(SCR_STACKED, true, DIATOM_SCALE_ASPECT_FIT, 256, 384, 640, 480, p);
	ck(is(p[0].src, 0, 192, 256, 192) && is(p[0].dst, 160, 0, 320, 240), "swapped: bottom screen on top");
	ck(is(p[1].src, 0, 0, 256, 192) && is(p[1].dst, 160, 240, 320, 240), "and the top screen under it");

	/* Integer: 1x, both screens whole. */
	n = scr_planes(SCR_STACKED, false, DIATOM_SCALE_INTEGER, 256, 384, 640, 480, p);
	ck(is(scr_bounds(p, n), 192, 48, 256, 384), "stacked integer is 1x, centered");

	/* Focus: 2x and a quarter of it, 640 wide together, the small one at
	 * the large one's bottom edge. */
	n = scr_planes(SCR_FOCUS, false, DIATOM_SCALE_ASPECT_FIT, 256, 384, 640, 480, p);
	ck(n == 2, "focus has two planes");
	ck(is(p[0].src, 0, 0, 256, 192) && is(p[0].dst, 0, 48, 512, 384), "focus: top screen 512x384 at 0,48");
	ck(is(p[1].src, 0, 192, 256, 192) && is(p[1].dst, 512, 336, 128, 96), "bottom 128x96 at 512,336");
	ck(is(scr_bounds(p, n), 0, 48, 640, 384), "focus bounds 640x384");
	n = scr_planes(SCR_FOCUS, true, DIATOM_SCALE_STRETCH, 256, 384, 640, 480, p);
	ck(is(p[0].src, 0, 192, 256, 192) && p[0].dst.w == 512, "focus swapped: the bottom screen is the large one");
	/* On a 1024x768 panel the fit is 3.2x; a whole-factor mode takes 3. */
	n = scr_planes(SCR_FOCUS, false, DIATOM_SCALE_INTEGER, 256, 384, 1024, 768, p);
	ck(p[0].dst.w == 768 && p[0].dst.h == 576 && p[1].dst.w == 192 && p[1].dst.h == 144,
	   "focus integer on 1024x768: 3x and 0.75x");

	/* Single: 4:3 on 4:3, so the panel exactly. */
	n = scr_planes(SCR_SINGLE, false, DIATOM_SCALE_ASPECT_FIT, 256, 384, 640, 480, p);
	ck(n == 1 && is(p[0].src, 0, 0, 256, 192) && is(p[0].dst, 0, 0, 640, 480), "single fills 640x480");
	n = scr_planes(SCR_SINGLE, true, DIATOM_SCALE_INTEGER, 256, 384, 640, 480, p);
	ck(n == 1 && is(p[0].src, 0, 192, 256, 192) && is(p[0].dst, 64, 48, 512, 384),
	   "single swapped, integer: the bottom screen at 2x");

	ck(scr_layout_index("focus") == SCR_FOCUS && scr_layout_index("side") == -1, "layout names");
	ck(scr_planes(SCR_STACKED, false, DIATOM_SCALE_ASPECT_FIT, 256, 1, 640, 480, p) == 0,
	   "a frame too short to cut gives no planes");
}

static void compose(void)
{
	static uint32_t frame[256 * 384];
	diatom_plane p[DIATOM_PLANES_MAX];
	uint32_t *pic;
	int n, w = 0, h = 0, x, y, bad = 0;

	/* The top screen one color, the bottom another, a marker in a corner. */
	for (y = 0; y < 384; y++)
		for (x = 0; x < 256; x++)
			frame[y * 256 + x] = y < 192 ? 0x112233u : 0x445566u;
	/* Four by four, as the small screen keeps one pixel in four each way. */
	for (y = 192; y < 196; y++)
		for (x = 0; x < 4; x++) frame[y * 256 + x] = 0xff0000u;

	n = scr_planes(SCR_STACKED, false, DIATOM_SCALE_ASPECT_FIT, 256, 384, 640, 480, p);
	pic = scr_compose(frame, 256 * 4, 4, p, n, &w, &h);
	ck(pic && w == 256 && h == 384, "stacked preview is the frame's own size");
	for (y = 0; pic && y < h; y++)
		for (x = 0; x < w; x++)
			if (pic[y * w + x] != frame[y * 256 + x]) bad++;
	ck(pic && !bad, "and the frame itself, pixel for pixel");
	free(pic);

	n = scr_planes(SCR_FOCUS, false, DIATOM_SCALE_ASPECT_FIT, 256, 384, 640, 480, p);
	pic = scr_compose(frame, 256 * 4, 4, p, n, &w, &h);
	ck(pic && w == 320 && h == 192, "focus preview 320x192");
	ck(pic && pic[0] == 0x112233u && pic[100 * 320 + 255] == 0x112233u, "top screen at 1x on the left");
	ck(pic && pic[191 * 320 + 300] == 0x445566u, "the bottom screen small at the right");
	ck(pic && pic[0 * 320 + 300] == 0, "black above it");
	ck(pic && pic[144 * 320 + 256] == 0xff0000u, "its corner marker where the small screen starts");
	free(pic);

	n = scr_planes(SCR_SINGLE, true, DIATOM_SCALE_ASPECT_FIT, 256, 384, 640, 480, p);
	pic = scr_compose(frame, 256 * 4, 4, p, n, &w, &h);
	ck(pic && w == 256 && h == 192 && pic[0] == 0xff0000u && pic[3] == 0xff0000u &&
	   pic[4] == 0x445566u,
	   "single preview is the shown screen, 256x192");
	free(pic);
}

static void stylus(void)
{
	static uint32_t frame[256 * 384], copy[256 * 384];
	int16_t px, py;
	int x, y, cx, cy, bad = 0;
	bool down;

	sty_reset(256, 192);
	ck(!sty_on(), "a new game starts with the stylus off");
	sty_cursor(&cx, &cy, NULL);
	ck(cx == 128 && cy == 96, "the cursor in the middle");
	sty_frame(STY_RIGHT, true);
	sty_cursor(&cx, NULL, &down);
	ck(cx == 128 && !down && !sty_pointer(&px, &py), "off, the d-pad and R2 do nothing");

	/* Every pixel of the bottom screen, through dsperate-lr's own reading
	 * of the pointer (src/libretro.cpp, retro_run). */
	sty_set(true);
	for (y = 0; y < 192; y++)
		for (x = 0; x < 256; x++) {
			int gx, gy;

			sty_place(x, y);
			sty_frame(0, true);
			sty_cursor(&cx, &cy, NULL);
			if (cx != x || cy != y) { bad++; continue; }
			if (!sty_pointer(&px, &py) || py < 0) { bad++; continue; }
			gx = (px + 0x7fff) * 256 / 0xffff;
			gy = py * 192 / 0x8000;
			if (gx != cx || gy != cy) bad++;
		}
	ck(!bad, "every bottom-screen pixel reads back as itself in dsperate-lr");

	/* Speed: 1 px a frame, then 2, then 4. */
	sty_reset(256, 192);
	sty_set(true);
	for (x = 0; x < 8; x++) sty_frame(STY_LEFT, false);
	sty_cursor(&cx, NULL, NULL);
	ck(cx == 120, "eight frames held move eight pixels");
	for (x = 0; x < 16; x++) sty_frame(STY_LEFT, false);
	sty_cursor(&cx, NULL, NULL);
	ck(cx == 88, "the next sixteen two each");
	sty_frame(STY_LEFT, false);
	sty_cursor(&cx, NULL, NULL);
	ck(cx == 84, "then four");
	for (x = 0; x < 100; x++) sty_frame(STY_LEFT | STY_UP, false);
	sty_cursor(&cx, &cy, NULL);
	ck(cx == 0 && cy == 0, "and it stops at the screen's edge");
	sty_frame(0, false);
	sty_frame(STY_RIGHT, false);
	sty_cursor(&cx, NULL, NULL);
	ck(cx == 1, "let go, it starts slow again");

	/* The cursor goes into the bottom half and comes out without a trace. */
	for (y = 0; y < 384; y++)
		for (x = 0; x < 256; x++) frame[y * 256 + x] = (uint32_t)(y * 256 + x) * 2654435761u;
	memcpy(copy, frame, sizeof frame);
	sty_reset(256, 192);
	sty_set(true);
	sty_frame(0, true);
	sty_stamp(frame, 256 * 4, 4, 256, 384);
	ck(frame[(192 + 96) * 256 + 128] == 0xff3030u, "the cursor's middle is red with the stylus down");
	ck(frame[(192 + 96 - 3) * 256 + 128] == 0xffffffu, "its arms white");
	ck(frame[(192 + 96) * 256 + 127 + 0] != copy[(192 + 96) * 256 + 127] , "drawn over the bottom screen");
	ck(!memcmp(frame, copy, 192 * 256 * 4), "the top screen untouched");
	sty_unstamp(frame, 256 * 4, 4);
	ck(!memcmp(frame, copy, sizeof frame), "and the frame exactly as it was after");

	/* At a corner it clips to the bottom screen, never into the top one. */
	for (x = 0; x < 100; x++) sty_frame(STY_LEFT | STY_UP, false);
	sty_stamp(frame, 256 * 4, 4, 256, 384);
	ck(!memcmp(frame, copy, 192 * 256 * 4), "clipped at the corner, the top screen still untouched");
	sty_unstamp(frame, 256 * 4, 4);
	ck(!memcmp(frame, copy, sizeof frame), "and put back");

	sty_set(false);
	sty_stamp(frame, 256 * 4, 4, 256, 384);
	ck(!memcmp(frame, copy, sizeof frame), "off, nothing is drawn");
}

int main(void)
{
	layouts();
	compose();
	stylus();
	printf("screens: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
