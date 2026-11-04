/* See screens.h. */
#include <stdlib.h>
#include <string.h>

#include "screens.h"

const char *const scr_layout_names[SCR_LAYOUTS] = { "stacked", "focus", "single" };

int scr_layout_index(const char *name)
{
	int i;

	for (i = 0; name && i < SCR_LAYOUTS; i++)
		if (!strcmp(name, scr_layout_names[i])) return i;
	return -1;
}

/* Whole factors only, for the modes that are about whole factors. A focus
 * layout on the Pixel 2 comes out at 2x for the large screen either way; on a
 * 1024x768 panel it is 3.2x by fit and 3x by these. */
static bool whole(diatom_scale_mode m)
{
	return m == DIATOM_SCALE_INTEGER || m == DIATOM_SCALE_INTEGER_VERT ||
	       m == DIATOM_SCALE_INTEGER_OVER || m == DIATOM_SCALE_NATIVE;
}

static diatom_rect rect(int x, int y, int w, int h)
{
	diatom_rect r = { x, y, w, h };
	return r;
}

int scr_planes(scr_layout layout, bool bottom_first, diatom_scale_mode mode,
               int frame_w, int frame_h, int surf_w, int surf_h,
               diatom_plane out[DIATOM_PLANES_MAX])
{
	const int sw = frame_w, sh = frame_h / 2;
	diatom_rect first  = rect(0, bottom_first ? sh : 0, sw, sh);
	diatom_rect second = rect(0, bottom_first ? 0 : sh, sw, sh);

	if (sw <= 0 || sh <= 0 || surf_w <= 0 || surf_h <= 0) return 0;

	switch (layout) {
	case SCR_FOCUS: {
		/* The large screen and a quarter-size one beside it, 1.25 screens
		 * wide together: as large as that fits, by width or by height. The
		 * small one sits at the large one's bottom edge, where a hand reaching
		 * for it on a real DS would find the lower screen. Not a scaled-down
		 * copy of the stacked picture: the reason for this layout is a top
		 * screen big enough to read, on a panel exactly 2.5 DS screens wide. */
		double b = (double)surf_w / (sw * 1.25), bh = (double)surf_h / sh;
		int bw, bhh, smw, smh, x0, y0;

		if (bh < b) b = bh;
		if (mode == DIATOM_SCALE_NATIVE) b = 1.0;
		else if (whole(mode) && b >= 1.0) b = (double)(int)b;
		bw  = (int)(sw * b + 0.5);
		bhh = (int)(sh * b + 0.5);
		smw = (int)(sw * b / 4.0 + 0.5);
		smh = (int)(sh * b / 4.0 + 0.5);
		if (smw < 1) smw = 1;
		if (smh < 1) smh = 1;
		x0 = (surf_w - bw - smw) / 2;
		y0 = (surf_h - bhh) / 2;
		out[0].src = first;
		out[0].dst = rect(x0, y0, bw, bhh);
		out[1].src = second;
		out[1].dst = rect(x0 + bw, y0 + bhh - smh, smw, smh);
		return 2;
	}
	case SCR_SINGLE:
		/* One screen, by the display mode, as though it were the whole
		 * frame: a DS screen is 4:3, so `aspect` and `stretch` are the same
		 * 640x480 on the Pixel 2. */
		out[0].src = first;
		out[0].dst = diatom_scale_rect(mode, sw, sh, (double)sw / sh, surf_w, surf_h);
		return 1;
	case SCR_STACKED:
	default: {
		/* The frame as one picture, which is what it is, and then cut: the
		 * two halves are where a plain present would have put them, so
		 * stacked unswapped draws exactly what Diatom drew before layouts. */
		diatom_rect all = diatom_scale_rect(mode, sw, 2 * sh, (double)sw / (2.0 * sh),
		                                    surf_w, surf_h);
		int top = all.h / 2;

		out[0].src = first;
		out[0].dst = rect(all.x, all.y, all.w, top);
		out[1].src = second;
		out[1].dst = rect(all.x, all.y + top, all.w, all.h - top);
		return 2;
	}
	}
}

diatom_rect scr_bounds(const diatom_plane *p, int n)
{
	int i, x0, y0, x1, y1;

	if (n <= 0) return rect(0, 0, 0, 0);
	x0 = p[0].dst.x; y0 = p[0].dst.y;
	x1 = x0 + p[0].dst.w; y1 = y0 + p[0].dst.h;
	for (i = 1; i < n; i++) {
		if (p[i].dst.x < x0) x0 = p[i].dst.x;
		if (p[i].dst.y < y0) y0 = p[i].dst.y;
		if (p[i].dst.x + p[i].dst.w > x1) x1 = p[i].dst.x + p[i].dst.w;
		if (p[i].dst.y + p[i].dst.h > y1) y1 = p[i].dst.y + p[i].dst.h;
	}
	return rect(x0, y0, x1 - x0, y1 - y0);
}

void *scr_compose(const void *frame, size_t pitch, int bytes_pp,
                  const diatom_plane *p, int n, int *w, int *h)
{
	diatom_rect b = scr_bounds(p, n);
	double k = 0.0;
	unsigned char *out;
	int i, cw, ch, x, y;

	if (!frame || n <= 0 || b.w <= 0 || b.h <= 0 || (bytes_pp != 2 && bytes_pp != 4))
		return NULL;
	for (i = 0; i < n; i++) {
		double s = p[i].src.w > 0 ? (double)p[i].dst.w / p[i].src.w : 0.0;
		if (s > k) k = s;
	}
	if (k <= 0.0) return NULL;
	cw = (int)(b.w / k + 0.5);
	ch = (int)(b.h / k + 0.5);
	if (cw <= 0 || ch <= 0) return NULL;
	out = calloc((size_t)cw * (size_t)ch, (size_t)bytes_pp);
	if (!out) return NULL;

	/* Each preview pixel's centre, carried to the surface and back into
	 * whichever plane it lands in. Later planes win, as they are drawn
	 * later. A pause costs this once; nothing per frame. */
	for (y = 0; y < ch; y++)
		for (x = 0; x < cw; x++) {
			double sx = b.x + (x + 0.5) * k, sy = b.y + (y + 0.5) * k;

			for (i = n - 1; i >= 0; i--) {
				const diatom_plane *q = &p[i];
				int fx, fy;

				if (sx < q->dst.x || sy < q->dst.y ||
				    sx >= q->dst.x + q->dst.w || sy >= q->dst.y + q->dst.h)
					continue;
				fx = q->src.x + (int)((sx - q->dst.x) * q->src.w / q->dst.w);
				fy = q->src.y + (int)((sy - q->dst.y) * q->src.h / q->dst.h);
				memcpy(out + ((size_t)y * cw + x) * bytes_pp,
				       (const unsigned char *)frame + (size_t)fy * pitch +
				           (size_t)fx * bytes_pp,
				       (size_t)bytes_pp);
				break;
			}
		}
	*w = cw;
	*h = ch;
	return out;
}

/* ---- the stylus -------------------------------------------------------- */

static bool g_on, g_down;
static int  g_x, g_y, g_w = 256, g_h = 192;
static int  g_held;          /* frames a direction has been held */

void sty_reset(int w, int h)
{
	g_on = g_down = false;
	g_w = w > 0 ? w : 256;
	g_h = h > 0 ? h : 192;
	g_x = g_w / 2;
	g_y = g_h / 2;
	g_held = 0;
}

void sty_set(bool on)
{
	g_on = on;
	if (!on) g_down = false;
	g_held = 0;
}

bool sty_on(void) { return g_on; }

void sty_frame(unsigned dirs, bool down)
{
	int step;

	if (!g_on) return;
	g_down = down;
	dirs &= STY_UP | STY_DOWN | STY_LEFT | STY_RIGHT;
	if (!dirs) { g_held = 0; return; }
	/* 1, then 2, then 4 pixels a frame: an eighth of a second for a single
	 * pixel's aim, then the 256 px of the screen in about a second. */
	step = g_held < 8 ? 1 : g_held < 24 ? 2 : 4;
	g_held++;
	if (dirs & STY_LEFT)  g_x -= step;
	if (dirs & STY_RIGHT) g_x += step;
	if (dirs & STY_UP)    g_y -= step;
	if (dirs & STY_DOWN)  g_y += step;
	if (g_x < 0) g_x = 0;
	if (g_y < 0) g_y = 0;
	if (g_x > g_w - 1) g_x = g_w - 1;
	if (g_y > g_h - 1) g_y = g_h - 1;
}

void sty_place(int x, int y)
{
	g_x = x < 0 ? 0 : x > g_w - 1 ? g_w - 1 : x;
	g_y = y < 0 ? 0 : y > g_h - 1 ? g_h - 1 : y;
}

void sty_cursor(int *x, int *y, bool *down)
{
	if (x) *x = g_x;
	if (y) *y = g_y;
	if (down) *down = g_on && g_down;
}

bool sty_pointer(int16_t *x, int16_t *y)
{
	/* The centre of the cursor's pixel, across the whole frame's
	 * -0x7fff..0x7fff: the bottom screen is the frame's lower half, so its y
	 * starts at 0. dsperate-lr reads x back as (x + 0x7fff) * 256 / 0xffff
	 * and y as y * 192 / 0x8000, which gives the same pixel at every one of
	 * the 256 x 192 (test/screens_test.c checks each). */
	*x = (int16_t)((2 * g_x + 1) * 0xffff / (2 * g_w) - 0x7fff);
	*y = (int16_t)((2 * g_y + 1) * 0x8000 / (2 * g_h));
	return g_on && g_down;
}

/* The cursor: a cross, five pixels each way, white with a black edge so it
 * reads on any picture, its middle red while the stylus is down. 13 x 13 at
 * most, saved before and put back after. */
#define CUR_R 6
#define CUR_D (2 * CUR_R + 1)
static unsigned char g_saved[CUR_D * CUR_D * 4];
static int  g_sx0, g_sy0, g_sw, g_sh;   /* the saved rect, in frame pixels */
static bool g_stamped;

static int cursor_px(int dx, int dy)
{
	int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;

	if ((dx == 0 && ay <= CUR_R - 1) || (dy == 0 && ax <= CUR_R - 1))
		return (ax <= 1 && ay <= 1) ? 3 : 2;   /* 3: the middle */
	if ((ax <= 1 && ay <= CUR_R) || (ay <= 1 && ax <= CUR_R))
		return 1;                               /* the black edge */
	return 0;
}

static void put(unsigned char *p, int bytes_pp, int kind)
{
	uint32_t c = kind == 1 ? 0x000000u : kind == 3 && g_down ? 0xff3030u : 0xffffffu;

	if (bytes_pp == 4) {
		uint32_t v = c;
		memcpy(p, &v, 4);
	} else {
		uint16_t v = (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
		memcpy(p, &v, 2);
	}
}

void sty_stamp(void *frame, size_t pitch, int bytes_pp, int frame_w, int frame_h)
{
	unsigned char *f = frame;
	int top = frame_h / 2, x, y, dx, dy;

	g_stamped = false;
	if (!g_on || !frame || (bytes_pp != 2 && bytes_pp != 4)) return;
	g_sx0 = g_x - CUR_R; g_sy0 = top + g_y - CUR_R;
	g_sw = CUR_D; g_sh = CUR_D;
	if (g_sx0 < 0) { g_sw += g_sx0; g_sx0 = 0; }
	if (g_sy0 < top) { g_sh -= top - g_sy0; g_sy0 = top; }
	if (g_sx0 + g_sw > frame_w) g_sw = frame_w - g_sx0;
	if (g_sy0 + g_sh > frame_h) g_sh = frame_h - g_sy0;
	if (g_sw <= 0 || g_sh <= 0) return;

	for (y = 0; y < g_sh; y++)
		memcpy(g_saved + (size_t)y * g_sw * bytes_pp,
		       f + (size_t)(g_sy0 + y) * pitch + (size_t)g_sx0 * bytes_pp,
		       (size_t)g_sw * bytes_pp);
	for (y = g_sy0; y < g_sy0 + g_sh; y++)
		for (x = g_sx0; x < g_sx0 + g_sw; x++) {
			int k;

			dx = x - g_x;
			dy = y - top - g_y;
			k = cursor_px(dx, dy);
			if (k) put(f + (size_t)y * pitch + (size_t)x * bytes_pp, bytes_pp, k);
		}
	g_stamped = true;
}

void sty_unstamp(void *frame, size_t pitch, int bytes_pp)
{
	unsigned char *f = frame;
	int y;

	if (!g_stamped || !frame) return;
	for (y = 0; y < g_sh; y++)
		memcpy(f + (size_t)(g_sy0 + y) * pitch + (size_t)g_sx0 * bytes_pp,
		       g_saved + (size_t)y * g_sw * bytes_pp, (size_t)g_sw * bytes_pp);
	g_stamped = false;
}
