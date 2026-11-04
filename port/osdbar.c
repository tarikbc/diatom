/* SPDX-License-Identifier: GPL-3.0-or-later OR MPL-2.0 */
/* Vendored from ZyperOS (TortOS) src/osdbar.c at 65f3f90, which is where it is changed:
 * the launcher, this port and portkeys draw the same bar from the same code. Copy
 * it back here whole when it changes there. */
/* The volume and brightness bar's pixels. See src/osdbar.h. */
#include "osdbar.h"

#include <math.h>
#include <string.h>

/* The pill inside the buffer, in units, and what is in it. */
#define PILL_X0  12.0f
#define PILL_Y0  12.0f
#define PILL_W   452.0f
#define PILL_H   64.0f
#define PILL_R   (PILL_H / 2)
#define BORDER   2.0f
#define ICON_CX  (PILL_X0 + 40.0f)
#define ICON_S   40.0f             /* the mark's box */
#define STROKE   4.0f              /* its lines */
#define SEG_X0   (PILL_X0 + 84.0f)
#define SEG_X1   (PILL_X0 + PILL_W - 30.0f)
#define SEG_H    16.0f
#define SEG_GAP  4.0f
#define SEG_R    3.0f
#define SHADOW_DY   DS_SHADOW_BAR_Y
#define SHADOW_SOFT DS_SHADOW_BAR_BLUR  /* units over which its edge fades */
#define BODY_A      0.93f          /* the game still reads through, faintly */

/* The colors are the design's (src/design.h), by role, in whichever theme the
 * caller names: the pill is a `surface` with an `edge` rim, a step not yet
 * reached is `track`, and the steps reached are one of two colors fixed per
 * function rather than taken from the system accent, which would make one
 * control change color as you scrolled past it - `light` for brightness,
 * `accent-ink` for volume. Accent-ink and not the accent: in Beard the accent
 * lime is 1.3:1 on the ground, a mark nobody could see, so the darker ink of
 * it is used, and in Salt & Pepper the two are the same lime. The shadow is
 * `ink` at the theme's own opacity, `bar_shadow`: 40% against a dark ground,
 * 18% on a light one, where a shadow is the only lift there is. */
static void rgb3(float out[3], ds_rgb c)
{
	out[0] = c.r;
	out[1] = c.g;
	out[2] = c.b;
}

/* ---- distances --------------------------------------------------------------
 *
 * Every shape is a signed distance: negative inside, in units. A pixel's
 * coverage is how much of a pixel-wide band around the edge it is past, which
 * is all the antialiasing there is, and it is the same at every scale. */

typedef struct {
	enum { SH_RBOX, SH_CIRCLE, SH_SEGMENT, SH_ARC, SH_QUAD } kind;
	float a, b, c, d, r;           /* box x0 y0 x1 y1 r; circle cx cy r;
	                                  segment x0 y0 x1 y1 half-width;
	                                  arc cx cy radius half-angle half-width */
	float q[8];                    /* quad, four corners, clockwise on screen */
} shape;

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float sd_rbox(const shape *s, float x, float y)
{
	float cx = (s->a + s->c) / 2, cy = (s->b + s->d) / 2;
	float hx = (s->c - s->a) / 2 - s->r, hy = (s->d - s->b) / 2 - s->r;
	float qx = fabsf(x - cx) - hx, qy = fabsf(y - cy) - hy;
	float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
	float in = qx > qy ? qx : qy;

	return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - s->r;
}

static float sd_segment(float x, float y, float x0, float y0, float x1, float y1)
{
	float dx = x1 - x0, dy = y1 - y0;
	float len2 = dx * dx + dy * dy;
	float t = len2 > 0 ? clampf(((x - x0) * dx + (y - y0) * dy) / len2, 0, 1) : 0;
	float ex = x - (x0 + dx * t), ey = y - (y0 + dy * t);

	return sqrtf(ex * ex + ey * ey);
}

static float sd(const shape *s, float x, float y)
{
	switch (s->kind) {
	case SH_RBOX:
		return sd_rbox(s, x, y);
	case SH_CIRCLE:
		return sqrtf((x - s->a) * (x - s->a) + (y - s->b) * (y - s->b)) - s->c;
	case SH_SEGMENT:
		return sd_segment(x, y, s->a, s->b, s->c, s->d) - s->r;
	case SH_ARC: {
		/* An arc of a circle opening to the right, round-capped: the ring's
		 * distance within the angle, the caps' outside it. */
		float px = x - s->a, py = y - s->b;
		float ang = atan2f(py, px);
		if (fabsf(ang) <= s->d)
			return fabsf(sqrtf(px * px + py * py) - s->c) - s->r;
		float ex = s->c * cosf(s->d), ey = s->c * sinf(s->d);
		float d0 = sqrtf((px - ex) * (px - ex) + (py + ey) * (py + ey));
		float d1 = sqrtf((px - ex) * (px - ex) + (py - ey) * (py - ey));
		return (d0 < d1 ? d0 : d1) - s->r;
	}
	case SH_QUAD: {
		/* Convex: the farthest of the four edges' outward distances. Exact
		 * inside and near enough just outside, which is all the edge needs. */
		float m = -1e9f;
		int i;
		for (i = 0; i < 4; i++) {
			float x0 = s->q[i * 2], y0 = s->q[i * 2 + 1];
			float x1 = s->q[(i + 1) % 4 * 2], y1 = s->q[(i + 1) % 4 * 2 + 1];
			float nx = y1 - y0, ny = x0 - x1;
			float l = sqrtf(nx * nx + ny * ny);
			float e = l > 0 ? ((x - x0) * nx + (y - y0) * ny) / l : 0;
			if (e > m) m = e;
		}
		return m;
	}
	}
	return 1e9f;
}

/* ---- painting ---------------------------------------------------------------
 *
 * Into a float buffer of premultiplied RGBA, one shape at a time over its own
 * bounds, with "over". The bar is small - 298 x 58 pixels on the panel - and
 * drawn only when the level changes, not every frame. */

typedef struct {
	float *f;                      /* w * h * 4 */
	int w, h;
	float scale;
} canvas;

static void bounds(const shape *s, float *x0, float *y0, float *x1, float *y1)
{
	switch (s->kind) {
	case SH_RBOX:    *x0 = s->a; *y0 = s->b; *x1 = s->c; *y1 = s->d; break;
	case SH_CIRCLE:  *x0 = s->a - s->c; *y0 = s->b - s->c;
	                 *x1 = s->a + s->c; *y1 = s->b + s->c; break;
	case SH_SEGMENT: *x0 = fminf(s->a, s->c) - s->r; *y0 = fminf(s->b, s->d) - s->r;
	                 *x1 = fmaxf(s->a, s->c) + s->r; *y1 = fmaxf(s->b, s->d) + s->r; break;
	case SH_ARC:     *x0 = s->a - s->c - s->r; *y0 = s->b - s->c - s->r;
	                 *x1 = s->a + s->c + s->r; *y1 = s->b + s->c + s->r; break;
	case SH_QUAD: {
		int i;
		*x0 = *x1 = s->q[0]; *y0 = *y1 = s->q[1];
		for (i = 1; i < 4; i++) {
			*x0 = fminf(*x0, s->q[i * 2]); *x1 = fmaxf(*x1, s->q[i * 2]);
			*y0 = fminf(*y0, s->q[i * 2 + 1]); *y1 = fmaxf(*y1, s->q[i * 2 + 1]);
		}
		break;
	}
	}
}

/* `soft` is the width of the edge in units: one pixel for a crisp shape,
 * more for the shadow. `inner`, when given, is a second color for the part
 * more than `inner_at` units inside the edge - the body's border. */
static void paint(canvas *cv, const shape *s, const float rgb[3], float a,
                  float soft, const float *inner, float inner_at)
{
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0, pad = soft + 1.0f / cv->scale;
	int px0, py0, px1, py1, x, y;

	bounds(s, &x0, &y0, &x1, &y1);
	px0 = (int)floorf((x0 - pad) * cv->scale);
	py0 = (int)floorf((y0 - pad) * cv->scale);
	px1 = (int)ceilf((x1 + pad) * cv->scale);
	py1 = (int)ceilf((y1 + pad) * cv->scale);
	if (px0 < 0) px0 = 0;
	if (py0 < 0) py0 = 0;
	if (px1 > cv->w) px1 = cv->w;
	if (py1 > cv->h) py1 = cv->h;
	for (y = py0; y < py1; y++)
		for (x = px0; x < px1; x++) {
			float ux = (x + 0.5f) / cv->scale, uy = (y + 0.5f) / cv->scale;
			float dist = sd(s, ux, uy);
			float cov = clampf(0.5f - dist / soft, 0, 1) * a;
			float c[3], *o;
			int k;

			if (cov <= 0) continue;
			for (k = 0; k < 3; k++) c[k] = rgb[k];
			if (inner) {
				float t = clampf(0.5f - (dist + inner_at) / soft, 0, 1);
				for (k = 0; k < 3; k++) c[k] = rgb[k] + (inner[k] - rgb[k]) * t;
			}
			o = cv->f + ((size_t)y * cv->w + x) * 4;
			for (k = 0; k < 3; k++) o[k] = c[k] / 255.0f * cov + o[k] * (1 - cov);
			o[3] = cov + o[3] * (1 - cov);
		}
}

static shape rbox(float x0, float y0, float x1, float y1, float r)
{
	return (shape){ .kind = SH_RBOX, .a = x0, .b = y0, .c = x1, .d = y1, .r = r };
}

static shape line(float x0, float y0, float x1, float y1)
{
	return (shape){ .kind = SH_SEGMENT, .a = x0, .b = y0, .c = x1, .d = y1, .r = STROKE / 2 };
}

/* The speaker: a box and its cone, then up to three waves, one more for each
 * third of the way up; a cross where the waves would be at silence. */
static void speaker(canvas *cv, float cx, float cy, int level, int max, const float *col, float px)
{
	const float s = ICON_S;
	shape body = rbox(cx - 0.55f * s, cy - 0.22f * s, cx - 0.25f * s, cy + 0.22f * s, 2);
	shape cone = { .kind = SH_QUAD, .q = {
		cx - 0.30f * s, cy - 0.22f * s,  cx + 0.05f * s, cy - 0.55f * s,
		cx + 0.05f * s, cy + 0.55f * s,  cx - 0.30f * s, cy + 0.22f * s } };
	int i, waves;

	paint(cv, &body, col, 1, px, NULL, 0);
	paint(cv, &cone, col, 1, px, NULL, 0);
	if (level <= 0) {
		float x0 = cx + 0.27f * s;
		shape a = line(x0, cy - 0.2f * s, x0 + 0.4f * s, cy + 0.2f * s);
		shape b = line(x0, cy + 0.2f * s, x0 + 0.4f * s, cy - 0.2f * s);
		paint(cv, &a, col, 1, px, NULL, 0);
		paint(cv, &b, col, 1, px, NULL, 0);
		return;
	}
	waves = max > 0 ? 1 + (level * 3 - 1) / max : 3;
	if (waves > 3) waves = 3;
	for (i = 0; i < waves; i++) {
		shape w = { .kind = SH_ARC, .a = cx + 0.05f * s, .b = cy,
		            .c = (0.28f + 0.24f * i) * s, .d = (float)M_PI / 4, .r = STROKE / 2 };
		paint(cv, &w, col, 1, px, NULL, 0);
	}
}

/* The sun: a disc and eight rays, longer the brighter. */
static void sun(canvas *cv, float cx, float cy, int level, int max, const float *col, float px)
{
	const float s = ICON_S;
	shape disc = { .kind = SH_CIRCLE, .a = cx, .b = cy, .c = 0.24f * s };
	float ray = s * (0.12f + 0.16f * (max > 0 ? (float)level / max : 1));
	int i;

	paint(cv, &disc, col, 1, px, NULL, 0);
	for (i = 0; i < 8; i++) {
		float ang = i * (float)M_PI / 4, c = cosf(ang), sn = sinf(ang);
		float r0 = 0.36f * s + STROKE / 2;
		shape r = line(cx + c * r0, cy + sn * r0, cx + c * (r0 + ray - STROKE / 2),
		               cy + sn * (r0 + ray - STROKE / 2));
		paint(cv, &r, col, 1, px, NULL, 0);
	}
}

void osdbar_size(float scale, int *w, int *h)
{
	*w = (int)ceilf(OSDBAR_W * scale);
	*h = (int)ceilf(OSDBAR_H * scale);
}

void osdbar_render(uint32_t *px, int w, int h, int stride, float scale,
                   int theme, int kind, int level, int max, float alpha, bool premul)
{
	/* The largest the bar is ever drawn, at scale 1: 476 x 92 pixels. */
	static float f[OSDBAR_W * OSDBAR_H * 4];
	canvas cv = { f, w, h, scale };
	const ds_theme *th;
	float col[3], track[3], surface[3], edge[3], ink[3];
	const float pix = 1.0f / scale;            /* a pixel, in units */
	const float cy = PILL_Y0 + PILL_H / 2;
	shape sh = rbox(PILL_X0, PILL_Y0 + SHADOW_DY, PILL_X0 + PILL_W, PILL_Y0 + PILL_H + SHADOW_DY, PILL_R);
	shape body = rbox(PILL_X0, PILL_Y0, PILL_X0 + PILL_W, PILL_Y0 + PILL_H, PILL_R);
	float sw;
	int i, x, y;

	if ((size_t)w * h > sizeof f / sizeof f[0] / 4) return;
	if (theme < 0 || theme >= DS_THEMES) theme = DS_LIGHT;
	th = &DS_THEME[theme];
	rgb3(col, kind == OSDBAR_BRIGHT ? th->light : th->accent_ink);
	rgb3(track, th->track);
	rgb3(surface, th->surface);
	rgb3(edge, th->edge);
	rgb3(ink, th->ink);
	if (max < 1) max = 1;
	if (level < 0) level = 0;
	if (level > max) level = max;
	memset(f, 0, (size_t)w * h * 4 * sizeof f[0]);

	paint(&cv, &sh, ink, th->bar_shadow / 255.0f, SHADOW_SOFT, NULL, 0);
	paint(&cv, &body, edge, BODY_A, pix, surface, BORDER);
	if (kind == OSDBAR_BRIGHT) sun(&cv, ICON_CX, cy, level, max, col, pix);
	else                       speaker(&cv, ICON_CX, cy, level, max, col, pix);

	/* One segment per step, widths shared out evenly across the track. */
	sw = (SEG_X1 - SEG_X0 - SEG_GAP * (max - 1)) / max;
	for (i = 0; i < max; i++) {
		float x0 = SEG_X0 + i * (sw + SEG_GAP);
		shape seg = rbox(x0, cy - SEG_H / 2, x0 + sw, cy + SEG_H / 2, SEG_R);
		paint(&cv, &seg, i < level ? col : track, 1, pix, NULL, 0);
	}

	if (alpha < 0) alpha = 0;
	if (alpha > 1) alpha = 1;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			const float *o = f + ((size_t)y * w + x) * 4;
			float a = o[3] * alpha, k = premul ? alpha : (o[3] > 0 ? 1 / o[3] : 0);
			uint32_t r = (uint32_t)(clampf(o[0] * k, 0, 1) * 255 + 0.5f);
			uint32_t g = (uint32_t)(clampf(o[1] * k, 0, 1) * 255 + 0.5f);
			uint32_t b = (uint32_t)(clampf(o[2] * k, 0, 1) * 255 + 0.5f);
			uint32_t A = (uint32_t)(clampf(a, 0, 1) * 255 + 0.5f);
			px[(size_t)y * stride + x] = A << 24 | r << 16 | g << 8 | b;
		}
}

/* The way in is the design's `out` curve, the quintic ease-out every arrival
 * uses (DESIGN.md 8): 87% of the way there a third into the time, so a press
 * shows at once and then settles. The way out is its `fade`, linear, the
 * opacity of something going away. */
bool osdbar_envelope(uint32_t since_shown, uint32_t since_change,
                     float *alpha, float *rise)
{
	float in = ds_out((float)since_shown / OSDBAR_IN_MS);
	float out = 1;

	if (since_change >= OSDBAR_HOLD_MS + OSDBAR_OUT_MS) return false;
	if (since_change > OSDBAR_HOLD_MS)
		out = 1 - (float)(since_change - OSDBAR_HOLD_MS) / OSDBAR_OUT_MS;
	*alpha = (in < out ? in : out);
	*rise = (1 - in) * OSDBAR_RISE;
	return true;
}

uint32_t osdbar_next_change(uint32_t since_shown, uint32_t since_change)
{
	if (since_shown < OSDBAR_IN_MS || since_change >= OSDBAR_HOLD_MS) return 0;
	return OSDBAR_HOLD_MS - since_change;
}
