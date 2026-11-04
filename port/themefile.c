/* SPDX-License-Identifier: GPL-3.0-or-later OR MPL-2.0 */
/* Vendored from ZyperOS (TortOS) src/themefile.c at 65f3f90, which is where it is changed:
 * the launcher writes the theme file and this port and portkeys read it with the
 * same code. Copy it back here whole when it changes there. */
/* The theme file. See src/themefile.h. */
#include "themefile.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif

/* Whether the n bytes at s, trimmed of whitespace at both ends, are exactly
 * `word`. By length and not by strcmp, so a NUL in the file is not the end of
 * the word: "dark\0junk" is not dark. */
static bool is_word(const char *s, size_t n, const char *word)
{
	size_t len = strlen(word);

	while (n && isspace((unsigned char)*s)) s++, n--;
	while (n && isspace((unsigned char)s[n - 1])) n--;
	return n == len && !memcmp(s, word, len);
}

int themefile_read(const char *path)
{
	/* A word and its whitespace is a dozen bytes. A file as long as this
	 * is not ours, and is not read further to find out: it is the default,
	 * which is also what makes the read one syscall, on every key press of
	 * a program that has no time to spare for a file. */
	char buf[32];
	ssize_t n;
	int fd, theme = DS_LIGHT;

	if (!path) return DS_LIGHT;
	/* O_NONBLOCK: a FIFO left at this name makes a plain open wait for a
	 * writer that may never come, on the key press that asked. With it the
	 * open returns at once and the read finds nothing, which is the default. */
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) return DS_LIGHT;
	n = read(fd, buf, sizeof buf);
	close(fd);
	if (n <= 0 || n >= (ssize_t)sizeof buf) return DS_LIGHT;

	if (is_word(buf, (size_t)n, "dark")) theme = DS_DARK;
	return theme;
}

bool themefile_write(const char *path, int theme)
{
	const char *text;
	char tmp[1024];
	size_t len;
	int fd, saved;
	bool ok;

	if (theme == DS_DARK) text = "dark\n";
	else if (theme == DS_LIGHT) text = "light\n";
	else { errno = EINVAL; return false; }
	if (!path) { errno = EINVAL; return false; }
	if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) {
		errno = ENAMETOOLONG;
		return false;
	}

	/* The temporary is made new each time, and not written through whatever
	 * is there: /tmp is every user's on a development host, and a link left
	 * at this name would have this write to its target. O_EXCL after an
	 * unlink refuses to follow one, and a stale temporary of a launcher that
	 * was killed mid-write is simply replaced. If somebody wins the race
	 * between the two, the open fails and the write is not made, which the
	 * caller logs and carries on from. */
	unlink(tmp);
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
	if (fd < 0) return false;

	/* No fsync: this is /tmp, a RAM filesystem on the device, and the file
	 * is a fact about this boot that the next start of the launcher writes
	 * again. Nothing is lost that a flush would have kept. */
	len = strlen(text);
	ok = write(fd, text, len) == (ssize_t)len;
	if (close(fd) != 0) ok = false;
	if (ok && rename(tmp, path) != 0) ok = false;
	if (!ok) {
		saved = errno;       /* the unlink must not be what the caller reads */
		unlink(tmp);
		errno = saved;
	}
	return ok;
}
