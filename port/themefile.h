/* SPDX-License-Identifier: GPL-3.0-or-later OR MPL-2.0 */
/* Vendored from ZyperOS src/themefile.h at 65f3f90, which is where it is changed:
 * the launcher writes the theme file and this port and portkeys read it with the
 * same code. Copy it back here whole when it changes there. */
#ifndef ZYPEROS_THEMEFILE_H
#define ZYPEROS_THEMEFILE_H

/* Which theme is on, as a file the other processes can read.
 *
 * The launcher knows the theme, and two programs draw the volume and
 * brightness bar (src/osdbar.c) without it: Diatom over a game, and portkeys
 * over a port while the launcher is stopped. There is no socket between
 * them to ask on, and none is worth building for one word, so the launcher
 * writes the word to THEME_FILE at start and whenever the theme changes, and
 * each of the others reads it as the bar comes up. /tmp, because it is a
 * fact about this boot and not a setting: the setting is the launcher's, in
 * its database, and is read again at every start, which writes this again.
 *
 * Two words, `light` and `dark` and a newline, for DS_LIGHT (Beard) and
 * DS_DARK (Salt & Pepper) of src/design.h. The words and not the indices,
 * so a person can look at the file, and edit it to try the bar in the other
 * theme, and so the order of the enum is not a promise two repositories
 * must keep.
 *
 * Reading never fails. After a cold boot the file is not there until the
 * launcher has started, and a port can be started straight from a boot, so
 * a missing file is an ordinary state and reads as DS_LIGHT, the default
 * theme; so does anything that is not one of the two words. What the reader
 * draws is a pill on the player's screen over a running game, and a pill in
 * the wrong theme is a smaller harm than a pill that is not there.
 *
 * Writing is a temporary beside the file and a rename, so a reader that
 * opens it while it is rewritten sees the old word or the new, never
 * half of one, and a launcher killed in the middle leaves the old word.
 *
 * Plain C, no SDL, and either license, like src/osdbar.c: Diatom, which
 * is MPL, carries a copy of this with it. */

#include <stdbool.h>

#include "design.h"

#define THEME_FILE "/tmp/zyperos-theme"

/* The theme `path` names: DS_LIGHT or DS_DARK, and DS_LIGHT when the file is
 * missing, cannot be read, or says anything but `light` or `dark` (with
 * whitespace around the word allowed, as a person editing it leaves). */
int themefile_read(const char *path);

/* Writes `theme` (DS_LIGHT or DS_DARK) to `path`, through path.tmp and a
 * rename. False, with errno from what failed and nothing left behind, if the
 * theme is neither, the path is too long for its temporary, or the write
 * could not be made; the file then still says what it did. */
bool themefile_write(const char *path, int theme);

#endif
