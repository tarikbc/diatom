/* SPDX-License-Identifier: GPL-3.0-or-later OR MPL-2.0 */
/* Vendored from ZyperOS (TortOS) src/osdbar.h, which is where it is changed: the
 * launcher, this port and portkeys draw the same bar from the same code. Copy
 * it back here whole when it changes there. */
#ifndef TORTOS_OSDBAR_H
#define TORTOS_OSDBAR_H

/* The volume and brightness bar, as pixels: one drawing for every place it
 * shows, so it is the same thing on the shelf, in a Diatom game and over a
 * port, rather than three hands copying one picture.
 *
 * A pill at the top center of the screen: the setting's mark on the left (a
 * speaker, with a cross at silence, or a sun whose rays grow with the level)
 * and one segment per step to its right, filled up to the level. The colors
 * are the old line's, fixed per function: cyan for volume, warm for
 * brightness. Segments rather than a smooth fill because the levels ARE
 * steps - twenty presses of the volume, eleven of the brightness - and a
 * player can count where they are and how far there is to go.
 *
 * Why pixels and not SDL draws: two of the three places have no SDL. Diatom
 * draws its overlay with GLES, and portkeys has only a KMS buffer on the
 * panel's cursor layer while a port owns the display. All three take this
 * function's output as it is - TortOS as a texture, Diatom as a GL
 * texture, portkeys straight into the scanout buffer, turned for the panel -
 * so the design is written down once. Plain C, no allocation the
 * caller does not see, nothing but libm.
 *
 * Under either license above: TortOS is GPL and Diatom, which carries a copy
 * of these two files in its port/, is MPL.
 *
 * The geometry is in DESIGN UNITS, the launcher's 1024x768 drawing space,
 * and `scale` is output pixels per unit: 1 for TortOS, 0.625 for the Pixel 2's
 * 640x480 panel. Every edge is antialiased from its distance to the pixel's
 * center, so it is as crisp at 0.625 as at 1. */

#include <stdbool.h>
#include <stdint.h>

enum { OSDBAR_BRIGHT = 1, OSDBAR_VOLUME = 2 };   /* plat_osd_show's kinds */

/* The buffer: the pill (452 x 64) and a margin around it its shadow falls
 * into, in units. OSDBAR_X/Y are where the buffer's top left goes on the
 * screen, which puts the pill 22 units under the top edge, centered. */
#define OSDBAR_W 476
#define OSDBAR_H 92
#define OSDBAR_X ((1024 - OSDBAR_W) / 2)
#define OSDBAR_Y 10

/* Its life, the same everywhere: in over OSDBAR_IN_MS, sliding down
 * OSDBAR_RISE units as it fades up; held until OSDBAR_HOLD_MS after the last
 * change; out over OSDBAR_OUT_MS. A change while it fades out brings it back
 * to full at once, where it is - no second slide for a bar already there. */
#define OSDBAR_IN_MS   120
#define OSDBAR_HOLD_MS 1500
#define OSDBAR_OUT_MS  240
#define OSDBAR_RISE    14

/* The buffer's size in pixels at `scale`. */
void osdbar_size(float scale, int *w, int *h);

/* Draws the bar into px, w x h from osdbar_size, `stride` pixels a row,
 * clearing what is there. Pixels are 0xAARRGGBB in a uint32_t - ARGB8888 to
 * both SDL and DRM on a little-endian machine. `premul` gives premultiplied
 * color, which the display controller's blending expects; SDL's default
 * blending wants it straight. `alpha` (0..1) is multiplied in, for a host
 * that fades by redrawing rather than by blending the whole thing. */
void osdbar_render(uint32_t *px, int w, int h, int stride, float scale,
                   int kind, int level, int max, float alpha, bool premul);

/* Where in its life the bar is, from the milliseconds since it appeared and
 * since the last change: false once it is gone, else its opacity (0..1) and
 * how far above its resting place it still is, in units (0..OSDBAR_RISE). */
bool osdbar_envelope(uint32_t since_shown, uint32_t since_change,
                     float *alpha, float *rise);

/* The milliseconds, from the same two clocks, until the bar next looks
 * different on its own: 0 while it moves, the time to the fade while it
 * holds. What a host that only redraws on change waits for. Meaningless once
 * osdbar_envelope says it is gone. */
uint32_t osdbar_next_change(uint32_t since_shown, uint32_t since_change);

#endif
