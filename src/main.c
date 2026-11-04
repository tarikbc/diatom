/* Diatom - a minimal libretro frontend.
 *
 * Standalone is the primary mode:  diatom --core X.so --rom game.nes
 * A host application driving a resident Diatom over a socket is an additional
 * mode, not the fundamental one. Designing for a program that stands alone is
 * a stricter test than designing for one embedder.
 *
 * The frame is copied inside the core's video callback and presented AFTER
 * retro_run returns. Cores emit video and audio from inside retro_run in an
 * order that varies, so presenting from the callback means committing to a
 * frame before knowing how much audio it produced.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "cheevos.h"
#include "diatom.h"
#include "levels.h"
#include "screens.h"

/* --- the frame the core last handed us ----------------------------------- */
static void       *g_frame;
static size_t      g_frame_cap;
static int         g_frame_w, g_frame_h;
static size_t      g_frame_pitch;
static bool        g_frame_fresh;

/* The state plane, defined below but reachable from the menu loop that comes
 * first. */
/* The session, up here because menu_pause reads its persistence paths. The
 * lifecycle comment lives with run_session_inner below, which is what actually
 * runs one. */
typedef struct diatom_session {
	const char   *core, *rom, *shot, *state_load, *state_exit;
	const char   *preview;    /* BMP of the frame, written on pause and exit */
	const char   *firmware;   /* what the launcher says this content needs */
	const char   *cheevos;    /* an achievement set to watch */
	int           console;    /* which console's address space it is written against */
	long          limit;
	int           mode;
	diatom_filter filter;
	bool          list_only;
	/* 2: the frame is two screens, top over bottom (RUN screens=2, a DS
	 * game). Layouts and the stylus apply only then; see screens.h. */
	int           screens;
	/* RUN frameskip=late: at 1x too, a frame that starts late is run with
	 * its drawing left out, when the core can do that. See the frame loop. */
	bool          late_skip;
	bool          stylus;     /* --stylus: start with stylus mode on (tests) */
	/* RUN power=: the clock ceiling the system keeps its speed under, a
	 * DIATOM_POWER_ level. See diatom_port_power and power_trouble. */
	int           power;
} diatom_session;

static bool state_plane_msg(const diatom_msg *m);
static void levels_forget(void);
static void apply_display(int mode, diatom_filter filter);

static diatom_core     *g_core;   /* resident, never unloaded */
static diatom_policy    g_policy;

/* An overlay image, from the launcher, to be composited over the game.
 *
 * Diatom has no font and is not getting one: drawing text means a face, a
 * layout, and opinions about wording, all of which belong to whoever is
 * saying something. So the launcher renders the line and this carries the
 * pixels. The port composites (diatom_port.h), for the reason the level bar
 * is drawn there too - the OSD is screen space, after scaling.
 *
 * The file is eight bytes of header and then BGRA rows:
 *
 *     "DTOV"  uint16 width LE  uint16 height LE  w*h*4 bytes
 *
 * Deliberately not BMP, which is the obvious choice and the wrong one here:
 * getting an alpha channel through BMP means BI_BITFIELDS, SDL will not write
 * it, and a notice with no alpha is a hard rectangle stamped over the game.
 * This is a private channel between two programs in one repository pair,
 * written and read within milliseconds, so it is worth being exactly what is
 * needed and saying so.
 *
 * The buffer is held HERE because the port borrows rather than copies - one
 * buffer instead of one per port. */
#define OVERLAY_MAX_W 1024
#define OVERLAY_MAX_H 256

static uint8_t *g_overlay;

static bool show_overlay(const char *path, unsigned ms)
{
	unsigned char hdr[8];
	FILE *f;
	size_t need;
	int w, h;

	if (!path || !*path || ms == 0) {
		diatom_port_overlay(NULL, 0, 0, 0);
		return true;
	}
	f = fopen(path, "rb");
	if (!f) return false;
	if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "DTOV", 4) != 0) {
		fclose(f);
		return false;
	}
	w = hdr[4] | (hdr[5] << 8);
	h = hdr[6] | (hdr[7] << 8);
	if (w <= 0 || h <= 0 || w > OVERLAY_MAX_W || h > OVERLAY_MAX_H) {
		fclose(f);
		return false;
	}
	need = (size_t)w * (size_t)h * 4;

	/* Replacing frees the previous one, so at most one is ever held. The port
	 * is cleared first: it is holding the pointer about to be freed. */
	diatom_port_overlay(NULL, 0, 0, 0);
	free(g_overlay);
	g_overlay = malloc(need);
	if (!g_overlay) { fclose(f); return false; }
	if (fread(g_overlay, 1, need, f) != need) {
		fclose(f);
		free(g_overlay);
		g_overlay = NULL;
		return false;
	}
	fclose(f);

	diatom_port_overlay(g_overlay, w, h, ms);
	return true;
}

/* The preview is the CORE'S frame, not the screen. Three reasons, none of
 * them taste: it is 8-30x smaller (a 256x224 frame against a 1024x768 panel),
 * it never contains the OSD bar or a menu, and it needs nothing from the port
 * - the host already holds the last frame for dupe handling. minarch's
 * previews are small for the same reason a launcher cares: it decodes one per
 * visible card, on a shelf of dozens. BMP because that is what ZyperOS already
 * reads, top-down rows because that is what BMP wants for a positive height...
 * negative height, rather - top-down needs biHeight < 0, and getting that
 * wrong renders every card upside down. */
static bool write_bmp(const char *path, const void *pixels, int w, int h,
                      size_t pitch)
{
	FILE *f;
	unsigned char hdr[54];
	int y, x;
	size_t row = (size_t)w * 3, pad = (4 - (row & 3)) & 3;
	unsigned int size = 54 + (unsigned int)((row + pad) * (size_t)h);

	if (!pixels || w <= 0 || h <= 0) return false;
	f = fopen(path, "wb");
	if (!f) return false;

	memset(hdr, 0, sizeof hdr);
	hdr[0] = 'B'; hdr[1] = 'M';
	hdr[2]  = (unsigned char)size;       hdr[3]  = (unsigned char)(size >> 8);
	hdr[4]  = (unsigned char)(size >> 16); hdr[5] = (unsigned char)(size >> 24);
	hdr[10] = 54;
	hdr[14] = 40;
	hdr[18] = (unsigned char)w; hdr[19] = (unsigned char)(w >> 8);
	hdr[22] = (unsigned char)h; hdr[23] = (unsigned char)(h >> 8);
	hdr[26] = 1;                          /* planes */
	hdr[28] = 24;                         /* bpp */
	fwrite(hdr, 1, sizeof hdr, f);

	/* Bottom-up, as positive-height BMP requires. */
	for (y = h - 1; y >= 0; y--) {
		const unsigned char *src = (const unsigned char *)pixels
		                         + (size_t)y * pitch;
		unsigned char px[3] = { 0, 0, 0 };

		for (x = 0; x < w; x++) {
			if (g_policy.pixfmt == DIATOM_PIX_RGB565) {
				unsigned v = ((const unsigned short *)src)[x];
				px[2] = (unsigned char)(((v >> 11) & 0x1f) * 255 / 31);
				px[1] = (unsigned char)(((v >>  5) & 0x3f) * 255 / 63);
				px[0] = (unsigned char)(( v        & 0x1f) * 255 / 31);
			} else {   /* XRGB8888 */
				unsigned v = ((const unsigned *)src)[x];
				px[2] = (unsigned char)(v >> 16);
				px[1] = (unsigned char)(v >> 8);
				px[0] = (unsigned char)v;
			}
			fwrite(px, 1, 3, f);
		}
		if (pad) { unsigned char z[3] = {0,0,0}; fwrite(z, 1, pad, f); }
	}
	fclose(f);
	return true;
}

/* Two screens laid out (screens.h): defined below, with the display mode. */
static bool         g_dual;
/* The pause preview's path while a game with two screens is paused, so a
 * layout changed from the launcher's menu can be shown there at once: the
 * preview of a layout is that layout's picture (write_preview), and the
 * menu draws it as its backdrop. NULL while running, when nothing is drawn
 * from it until the next pause writes it anyway. */
static const char  *g_paused_preview;
static diatom_plane g_planes[DIATOM_PLANES_MAX];
static int          g_nplanes;

/* What is on the panel, at the game's own scale. For a DS layout that is not
 * the frame: a single screen is half of it, and focus puts the second screen
 * beside the first at a quarter of the size. The launcher draws the paused
 * frame into DISPLAY's rect, which for a layout is the planes' bounds, so the
 * preview is that rect's picture, composed (scr_compose) - the in-game menu
 * then sits over what the player was looking at, not over a tall strip
 * squeezed into the wrong box. Once per pause and once at exit. */
/* A GPU frame (hw.c) is no pixels until asked: read back into g_frame as
 * XRGB8888, top row first, for a preview. Once per pause and once at exit,
 * like the preview itself, so the cost of the read never reaches a frame. */
static void gpu_snapshot(void)
{
	size_t need = (size_t)g_frame_w * (size_t)g_frame_h * 4;
	int w = 0, h = 0;

	if (!diatom_hw_active() || need == 0) return;
	if (need > g_frame_cap) {
		void *p = realloc(g_frame, need);
		if (!p) return;
		g_frame = p;
		g_frame_cap = need;
	}
	if (!diatom_port_gpu_read(g_frame, &w, &h, g_frame_cap)) return;
	g_frame_w = w;
	g_frame_h = h;
	g_frame_pitch = (size_t)w * 4;
	g_policy.pixfmt = DIATOM_PIX_XRGB8888;
}

static bool write_preview(const char *path)
{
	gpu_snapshot();
	if (g_dual && g_nplanes > 0 && g_frame) {
		int bpp = g_policy.pixfmt == DIATOM_PIX_RGB565 ? 2 : 4, w = 0, h = 0;
		void *pic = scr_compose(g_frame, g_frame_pitch, bpp, g_planes, g_nplanes, &w, &h);
		bool ok = pic && write_bmp(path, pic, w, h, (size_t)w * (size_t)bpp);

		free(pic);
		if (ok) return true;
	}
	return write_bmp(path, g_frame, g_frame_w, g_frame_h, g_frame_pitch);
}

static diatom_port_caps g_caps;
static diatom_rect      g_dst;   /* locked at load */

/* Set from a signal handler and read by the frame loop. The handler does NO
 * file I/O: it flips this, the loop notices after retro_run returns and does
 * the flush itself. Measured on the Brick, SIGTERM arrives about 810 ms before
 * the process dies on power-off, and one frame of retro_run is ~10 ms of that
 * budget - a 1% price for not calling malloc or open from a handler. */
static volatile sig_atomic_t g_terminate;
static bool g_quit_requested;   /* launcher said QUIT */

/* The pipe that lets a blocked poll notice the flag above. See
 * diatom_proto_wake_fd - the flag alone is not enough, because the signal may
 * be delivered to any of SDL's threads while the main one sits in poll(-1). */
static int g_wake_pipe[2] = { -1, -1 };

static void on_terminate(int sig)
{
	(void)sig;
	g_terminate = 1;
	/* write() is on the async-signal-safe list; one byte down a non-blocking
	 * pipe, and a full pipe means a wake is already pending, so the failure
	 * case is the case that needed nothing. The result is read to keep the
	 * compiler quiet about a return value there is genuinely nothing to do
	 * about. */
	if (g_wake_pipe[1] >= 0) {
		ssize_t rc = write(g_wake_pipe[1], "", 1);
		(void)rc;
	}
}

/* Called once, before anything can be signaled.
 *
 * The per-session sigaction further down is about saving a game's state on
 * power-off. This is about the process being able to exit at all, which an
 * idle one could not: that sigaction is the ONLY install there was, and it
 * runs as part of starting a game, so a Diatom that had not run one had no
 * handler of its own at all.
 *
 * Measured on the device 2026-08-30, on a Diatom sitting at the shelf:
 * SigCgt had SIGTERM but not SIGHUP, and this function installs all three -
 * so what was catching SIGTERM was SDL's own handler, which sets an SDL quit
 * flag that the idle loop below never looks at. SIGTERM was delivered,
 * caught, and swallowed; the main thread stayed in poll_schedule_timeout
 * through six seconds of waiting and every attempt after that. The pre-fix
 * binary reproduces it on a desktop, which is where the two halves of this -
 * an install that happens early enough, and a poll that can be woken - were
 * checked against each other. */
static void terminate_init(void)
{
	struct sigaction sa;

	if (pipe(g_wake_pipe) == 0) {
		fcntl(g_wake_pipe[0], F_SETFL, O_NONBLOCK);
		fcntl(g_wake_pipe[1], F_SETFL, O_NONBLOCK);
		fcntl(g_wake_pipe[0], F_SETFD, FD_CLOEXEC);
		fcntl(g_wake_pipe[1], F_SETFD, FD_CLOEXEC);
		diatom_proto_wake_fd(g_wake_pipe[0]);
	}

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_terminate;
	/* No SA_RESTART, deliberately: a poll that resumes where it left off is
	 * a poll that never returns to the loop that would read the flag. */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGHUP,  &sa, NULL);
}

/* --- crash reporting - `EXIT reason=crash` -------------------------------- */

/* A core is dlopen'd into this address space, so a core that dies kills
 * Diatom. That rules out the trick on_terminate uses: there is no "after
 * retro_run returns" to defer the work to, because the process is going away
 * inside the handler.
 *
 * Which side of RUNNING the crash lands on decides the message, and that comes
 * straight from proto.c's rule rather than from taste. A core that dies while
 * loading means the game never started, so the launcher is still drawing and
 * must be told ERROR. A core that dies mid-game means it ran and stopped, so
 * the launcher is NOT drawing and must be told EXIT to take the display back.
 * Sending the wrong one leaves a handheld showing a dead frame.
 *
 * What this deliberately does not do is try to survive. Continuing after a
 * fault in a dlopen'd core - longjmp back to the protocol loop, accept the next
 * RUN - is tempting precisely because residency makes the process valuable, and
 * it is wrong: the address space is already suspect, the core's own state is
 * arbitrary, and the next game would inherit both. */
enum { PHASE_IDLE, PHASE_LOADING, PHASE_RUNNING };
static volatile sig_atomic_t g_phase;

/* Complete lines, one per signal per phase, because a handler cannot build a
 * string: snprintf is not async-signal-safe. Repetitive on purpose - the
 * alternative is assembling the line in pieces, and a torn write would put a
 * malformed message on the socket at the worst possible moment.
 *
 * `signal=` is an extra key, which the protocol makes free: unknown keys are
 * ignored, so an older launcher sees a plain crash and a newer one can log
 * which signal it was. That name is the only trace that survives - the process
 * is about to die, and nothing else records it. */
static const struct {
	int         sig;
	const char *running;   /* crashed after RUNNING: it ran and stopped */
	const char *loading;   /* crashed before RUNNING: it never started */
} crash_lines[] = {
	{ SIGSEGV, "EXIT\treason=crash\tsignal=SIGSEGV\n", "ERROR\tcode=crash\tmsg=SIGSEGV\n" },
	{ SIGBUS,  "EXIT\treason=crash\tsignal=SIGBUS\n",  "ERROR\tcode=crash\tmsg=SIGBUS\n"  },
	{ SIGILL,  "EXIT\treason=crash\tsignal=SIGILL\n",  "ERROR\tcode=crash\tmsg=SIGILL\n"  },
	{ SIGFPE,  "EXIT\treason=crash\tsignal=SIGFPE\n",  "ERROR\tcode=crash\tmsg=SIGFPE\n"  },
	{ SIGABRT, "EXIT\treason=crash\tsignal=SIGABRT\n", "ERROR\tcode=crash\tmsg=SIGABRT\n" },
};

/* Deliberately no diatom_port_present_stop() below. It waits on a condition
 * variable, which is not async-signal-safe, and a crashed game that tears on
 * its way out is a far better outcome than one that deadlocks in a signal
 * handler with the display held. The tear is milliseconds; the deadlock would
 * be a power cycle. */
static void on_crash(int sig)
{
	size_t i;

	/* PHASE_IDLE says no game was in flight, so there is nothing to report
	 * about one. Diatom dying between sessions reaches the launcher as the
	 * socket closing, which the protocol already relies on for liveness. */
	if (g_phase != PHASE_IDLE)
		for (i = 0; i < sizeof crash_lines / sizeof crash_lines[0]; i++)
			if (crash_lines[i].sig == sig) {
				diatom_proto_emit_fatal(g_phase == PHASE_RUNNING
				                        ? crash_lines[i].running
				                        : crash_lines[i].loading);
				break;
			}

	/* Then die of the original signal with its original disposition. Anything
	 * else lies about how the process ended: the wait status is how a
	 * supervisor tells a crash from a clean stop, and the core dump is how
	 * anyone finds out why. Returning instead would re-run the faulting
	 * instruction and loop here forever.
	 *
	 * signal() and raise() are both on the async-signal-safe list; sigaction()
	 * with a memset struct is not, which is why the older-looking call is the
	 * correct one here. */
	signal(sig, SIG_DFL);
	raise(sig);
}

/* A core that calls exit() on a fatal error - some do - raises no signal, so
 * the handler above never runs and the launcher would wait forever on a
 * process that has already gone. Not signal context, so the ordinary send is
 * fine here.
 *
 * Fires on every exit including the clean ones, which is what PHASE_IDLE is
 * for: after a normal EXIT there is nothing left to say. */
static void on_exit_hook(void)
{
	if (g_phase == PHASE_IDLE) return;
	if (g_phase == PHASE_RUNNING)
		diatom_proto_send("EXIT\treason=crash\tmsg=core called exit");
	else
		diatom_proto_send("ERROR\tcode=crash\tmsg=core called exit");
}

/* 64 KB, a fixed size rather than SIGSTKSZ: since glibc 2.34 that expands to a
 * sysconf() call, which cannot size an array at file scope, and the Brick
 * builds against a newer glibc than the desktop does. */
static char g_crash_stack[65536];

static void install_crash_handlers(void)
{
	static stack_t ss;
	struct sigaction sa;
	size_t i;

	/* A private stack, because the failure most likely to arrive here is a
	 * core recursing without bound, and a SIGSEGV raised by an exhausted stack
	 * cannot run a handler on that same stack.
	 *
	 * MEASURED on the Brick, 2026-08-25, A/B on one binary an hour apart -
	 * stubcore recursing until the 8 MB stack is gone:
	 *
	 *   SA_ONSTACK      EXIT reason=crash signal=SIGSEGV
	 *   without it      nothing; the launcher saw only the socket close
	 *
	 * So this is load-bearing rather than defensive. Both runs still died on
	 * signal 11 - what the flag buys is the report, not the death.
	 *
	 * Per-thread, so this covers the main thread only. The port's flip thread
	 * does not recurse; a fault there still reports, just not the overflow. */
	ss.ss_sp    = g_crash_stack;
	ss.ss_size  = sizeof g_crash_stack;
	ss.ss_flags = 0;
	sigaltstack(&ss, NULL);

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_crash;
	sa.sa_flags   = SA_ONSTACK;
	/* Block everything for the duration: a second fault inside the handler
	 * then cannot re-enter it, and the kernel kills us outright instead. That
	 * is the right outcome - it is already the second failure. */
	sigfillset(&sa.sa_mask);
	for (i = 0; i < sizeof crash_lines / sizeof crash_lines[0]; i++)
		sigaction(crash_lines[i].sig, &sa, NULL);

	atexit(on_exit_hook);
}

/* How long a rect has to survive before it counts as the real mode. Herzog
 * Zwei, the slowest observed, corrects at frame 29; three seconds is two orders
 * of margin over that and still far short of anything a person would reach by
 * opening a menu. */
#define SETTLE_FRAMES 180

/* Display mode state. The BASE geometry is kept because that, not the current
 * geometry, is what a rect is ever computed from: the locked rect's invariant
 * is "never recompute from a mid-run SET_GEOMETRY", which a deliberate
 * user-driven mode change does not violate as long as it recomputes from the
 * base. */
static int           g_base_w, g_base_h;
static double        g_base_aspect;
static int           g_mode;
static diatom_filter g_filter;

/* The DS layout, and whether the bottom screen goes first. Held across games,
 * as the display mode is, and set by the launcher after each RUN
 * (SETSCREENS); MENU+L1 swaps in a game. Applies only while g_dual. */
static scr_layout    g_layout = SCR_STACKED;
static bool          g_swap;

/* What presenting cost this session, for the summary at exit. */
static long     g_present_frames;
static uint64_t g_present_us;

/* The frame turned by the game's rotation (SET_ROTATION, see env.c), written
 * packed into g_frame. Libretro's quarter turns are counter-clockwise: a
 * source pixel (x, y) in a w x h frame lands at
 *
 *   1  (y, w-1-x)        in an h x w frame
 *   2  (w-1-x, h-1-y)    in a w x h frame
 *   3  (h-1-y, x)        in an h x w frame
 *
 * Walked in DESTINATION order, so the writes are sequential and only the
 * reads stride; on the A35 the read side is the one the cache forgives. A
 * vertical CPS frame is 384x224, 86,016 pixels, about the cost of the memcpy
 * it replaces times a small factor - nothing beside the core's own frame. */
#define ROTATE_LOOP(T)                                                      \
	do {                                                                    \
		const unsigned char *s = data;                                      \
		T *d = g_frame;                                                     \
		unsigned dx, dy;                                                    \
		for (dy = 0; dy < dh; dy++)                                         \
			for (dx = 0; dx < dw; dx++) {                                   \
				unsigned sx, sy;                                            \
				if (rot == 1)      { sx = w - 1 - dy; sy = dx; }            \
				else if (rot == 2) { sx = w - 1 - dx; sy = h - 1 - dy; }    \
				else               { sx = dy;         sy = h - 1 - dx; }    \
				*d++ = ((const T *)(s + (size_t)sy * pitch))[sx];           \
			}                                                               \
	} while (0)

static void store_rotated(const void *data, unsigned w, unsigned h,
                          size_t pitch, unsigned rot)
{
	unsigned dw = (rot & 1) ? h : w, dh = (rot & 1) ? w : h;

	if (g_policy.pixfmt == DIATOM_PIX_RGB565) {
		ROTATE_LOOP(uint16_t);
		g_frame_pitch = (size_t)dw * 2;
	} else {
		ROTATE_LOOP(uint32_t);
		g_frame_pitch = (size_t)dw * 4;
	}
	g_frame_w = (int)dw;
	g_frame_h = (int)dh;
}

/* Called from inside retro_run. Copy and return; do not present here. */
void diatom_on_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
	unsigned rot = diatom_env_rotation();
	size_t need;

	if (!data) return;          /* dupe: keep the previous frame */
	/* Drawn by the GPU into the port's framebuffer (hw.c): no pixels to copy,
	 * only the size. Not turned: no GPU core here asks for a rotation. */
	if (data == RETRO_HW_FRAME_BUFFER_VALID) {
		g_frame_w = (int)w;
		g_frame_h = (int)h;
		g_frame_fresh = true;
		return;
	}
	/* A turned frame is stored packed, which is never more than pitch * h,
	 * so one size covers both and the buffer still only grows - the frame
	 * loop allocates nothing once it has seen its largest frame. */
	need = pitch * h;
	if (need > g_frame_cap) {
		void *p = realloc(g_frame, need);
		if (!p) return;
		g_frame = p;
		g_frame_cap = need;
	}
	if (rot) {
		store_rotated(data, w, h, pitch, rot);
	} else {
		memcpy(g_frame, data, need);
		g_frame_w = (int)w;
		g_frame_h = (int)h;
		g_frame_pitch = pitch;
	}
	g_frame_fresh = true;
}

/* Raised for the warmup frames only. Their audio is pre-roll for a game that
 * has not started, produced by three unpaced calls to retro_run, and queueing
 * it is what filled the buffer before the paced loop ever ran:
 *
 *   prime to half   2048
 *   warmup frame 1  3008
 *   warmup frame 2  3968
 *   warmup frame 3  4096, and 831 of 960 frames refused
 *
 * Measured on the Brick 2026-08-25 - 4120 frames dropped at every launch,
 * identical at 300, 600, 1200 and 2400 frames of run, because it is entirely a
 * startup transient. Dropping this audio deliberately is more honest than
 * queueing it and then having the port refuse it. */
static bool g_audio_warmup;
/* Fast forward is muted. The core's sound is dropped here, and the frame
 * loop pushes silence for the time each fast turn stands for. */
static bool g_audio_fast;

/* A core that makes its sound on a thread of its own pushes it from there:
 * flycast2021's threaded rendering does, from its emulation thread. audio.c
 * takes a lock for it.
 *
 * And it is that thread's pace. Nothing else holds it back - RetroArch's
 * sound write blocks, which is what holds it there - and unheld it ran
 * Crazy Taxi 2 31% fast, its sound dropped by the second and frames it had
 * no time to hand over thrown away (2026-11-04). So it waits while the
 * device's queue is over three quarters full, which holds the game to the
 * speaker's rate: the queue drains on its own, whatever the frame loop is
 * doing. (Held to sound the frame loop moved, it waited on a frame that was
 * waiting on it: 72% of real time.) Never past a bound, so a stopped device
 * only loses that thread some sound. Not in fast forward, which is meant to
 * outrun the speaker; the loop pushes silence then. */
#define FOREIGN_WAIT_US 100000
#define FOREIGN_POLL_US 2000
static pthread_t g_host_thread;
static size_t    g_foreign_high;       /* three quarters of the device's queue; 0 none */

static void foreign_pace(void)
{
	unsigned waited = 0;

	if (!g_foreign_high || g_audio_fast) return;
	while (diatom_port_audio_queued() > g_foreign_high && waited < FOREIGN_WAIT_US) {
		usleep(FOREIGN_POLL_US);
		waited += FOREIGN_POLL_US;
	}
}

void diatom_on_audio_batch_store(const int16_t *data, size_t frames)
{
	if (!pthread_equal(pthread_self(), g_host_thread)) {
		foreign_pace();
		if (getenv("DIATOM_AUDIO_DEBUG")) {
			static uint64_t since, got;
			uint64_t now = diatom_port_now_us();

			got += frames;
			if (!since) since = now;
			if (now - since >= 2000000) {
				printf("diatom: %.0f sound frames a second from the core's thread, queue %zu\n",
				       got * 1e6 / (double)(now - since), diatom_port_audio_queued());
				got = 0;
				since = now;
			}
		}
	}
	if (g_audio_fast) return;    /* the frame loop pushes silence instead */
	diatom_audio_note_input(data, frames);
	if (g_audio_warmup) return;
	diatom_audio_push(data, frames);
}

static void usage(void)
{
	int i;
	fprintf(stderr,
		"usage: diatom --core <core.so> --rom <file> [--system <dir>] [--save <dir>]\n"
		"              [--display <mode>] [--filter nearest|sharp]\n"
		"              [--channels stereo|mono|left|right]\n"
		"              [--load-state <file>] [--state-on-exit <file>]\n"
		"              [--firmware <name>]  required in --system, checked first\n"
		"              [--frames <n>] [--shot <file.bmp>] [--preview-on-exit <file.bmp>]\n"
		"              [--socket <path>]   launcher protocol\n"
		"              [--cores <dir>]     map every core there before listening\n"
		"              [--core-option key=value] ...   repeatable\n"
		"              [--list-options]    what this core offers, then exit\n"
		"              [--screens 2]       the frame is two screens, top over bottom (DS)\n"
		"              [--layout stacked|focus|single] [--swap] [--stylus]\n"
		"              [--frameskip late]  leave a late frame undrawn, at 1x too\n"
		"              [--power light|medium]  run under that clock ceiling\n"
		"\n--firmware names a file the content needs, e.g. syscard3.pce for a PC\n"
		"Engine CD. Diatom does not know which content needs what and will not\n"
		"learn - it has no core list - so whoever launches it says.\n"
		"\nSRAM is automatic: read at load, written when it changes, flushed on\n"
		"exit and on SIGTERM. Save states take paths, never slot numbers - slots\n"
		"belong to the launcher.\n\n");
	for (i = 0; i < diatom_mode_count; i++)
		fprintf(stderr, "  %-10s %s\n",
		        diatom_modes[i].name, diatom_modes[i].note);
}

static const char *filter_name(diatom_filter f)
{
	return f == DIATOM_FILTER_SHARP ? "sharp" : "nearest";
}

static void apply_display(int mode, diatom_filter filter)
{
	diatom_rect shown;
	int i;

	g_mode   = mode;
	g_filter = filter;
	g_dst = diatom_scale_rect(diatom_modes[mode].mode, g_base_w, g_base_h,
	                          g_base_aspect, g_caps.surface_w, g_caps.surface_h);
	shown = g_dst;
	/* Two screens: each half where the layout puts it, and what the launcher
	 * hears as the rect is where they are together. The stylus puts the
	 * bottom screen first in focus and single, so the screen it touches is
	 * the large one, or the one shown. */
	g_nplanes = 0;
	if (g_dual)
		g_nplanes = scr_planes(g_layout,
		                       g_layout == SCR_STACKED ? g_swap : (g_swap || sty_on()),
		                       diatom_modes[mode].mode, g_base_w, g_base_h,
		                       g_caps.surface_w, g_caps.surface_h, g_planes);
	if (g_nplanes) shown = scr_bounds(g_planes, g_nplanes);
	diatom_port_planes(g_planes, g_nplanes);
	printf("diatom: display %-10s %-7s %4dx%-4d at %4d,%-4d  %s\n",
	       diatom_modes[mode].name, filter_name(filter),
	       shown.w, shown.h, shown.x, shown.y, diatom_modes[mode].note);
	for (i = 0; i < g_nplanes; i++)
		printf("diatom: screens %-7s %s screen %3dx%-3d -> %4dx%-4d at %4d,%-4d\n",
		       scr_layout_names[g_layout], g_planes[i].src.y ? "bottom" : "top",
		       g_planes[i].src.w, g_planes[i].src.h,
		       g_planes[i].dst.w, g_planes[i].dst.h, g_planes[i].dst.x, g_planes[i].dst.y);
	fflush(stdout);

	/* Reported from the ONE place the mode ever changes, so a launcher hears
	 * about it whether it asked or the rect settled underneath it. `rect=` is
	 * free here and is what a launcher would otherwise have to recompute from
	 * geometry it does not have.
	 *
	 * `surface=` says what the rect is measured in. A launcher that draws at
	 * another size scales the rect to its own: on the GKD Pixel 2 the surface
	 * is 640x480 under ZyperOS's 1024x768, and the in-game menu drew the paused
	 * frame in the top-left 640x480 of the screen until it was said. */
	if (diatom_proto_connected())
		diatom_proto_send("DISPLAY\tmode=%s\tfilter=%s\trect=%dx%d+%d+%d\tsurface=%dx%d",
		                  diatom_modes[mode].name, filter_name(filter),
		                  shown.w, shown.h, shown.x, shown.y,
		                  g_caps.surface_w, g_caps.surface_h);
}

/* The in-game menu handover.
 *
 * MENU pauses the game and gives the display to the launcher, which draws its
 * own menu and answers. Diatom draws nothing: it has no UI, and the handoff
 * spike proved an fbdev presenter and an EGL launcher can alternate safely so
 * long as only one presents at a time.
 *
 * Blocks until the launcher answers, because a paused game is paused - running
 * the core would advance a world the player cannot see.
 *
 * Returns false if the session should end. */
/* The launcher asking for the menu. Exists for one case that has no other
 * answer: coming back after a shutdown, where the game has to be loaded and
 * the menu has to be up, and nobody pressed anything. Edge-consumed, so a
 * launcher that sends it twice gets one menu. */
static bool g_pause_requested;

/* Idleness, for the launcher's auto-off.
 *
 * Reported rather than acted on: Diatom does not decide that a device should
 * turn itself off, and does not know whether it is on a charger or what the
 * player set. It knows one thing the launcher cannot see - whether anybody is
 * pressing anything - because it owns the pad while a game runs and the
 * launcher is blocked on a socket. The launcher drives.
 *
 * A held button counts. Reading a map with a direction pressed is somebody
 * being there.
 *
 * IDLE and nothing else. There is no ACTIVE, because nothing would consume
 * one: the launcher acts on IDLE at once rather than warning first, since
 * powering off costs about two seconds and lands the player back in the same
 * game with the menu up. A window to cancel is only worth having when the
 * thing being canceled is expensive. */
static unsigned g_idle_after_ms;      /* 0: not asked for */
static uint64_t g_idle_since_us;
static bool     g_idle_said;

/* Start the count again from now.
 *
 * For the stretches when this loop is not running and somebody is plainly
 * there anyway - the in-game menu, where the player is operating the
 * LAUNCHER's pad and nothing here polls it. The clock is a timestamp, so it
 * goes on advancing through a pause it is not being read across, and the
 * first read after the menu closes would see the whole menu as idle time and
 * power the device off in the frame after RESUME.
 *
 * Safe when nobody asked for idle reporting: idle_note_input returns before
 * it looks at any of this. */
static void idle_restart(void)
{
	g_idle_since_us = diatom_port_now_us();
	g_idle_said = false;
}

static void idle_note_input(bool any)
{
	if (!g_idle_after_ms) return;

	if (any) {
		g_idle_since_us = diatom_port_now_us();
		g_idle_said = false;
		return;
	}
	if (!g_idle_said && g_idle_since_us &&
	    diatom_port_now_us() - g_idle_since_us >
	        (uint64_t)g_idle_after_ms * 1000ull) {
		g_idle_said = true;
		diatom_proto_send("IDLE");
	}
}

/* Fast forward. The step the player chose with MENU+R1, 1 to 4, reset at
 * every RUN and kept across the menu. A state on the state plane that only
 * Diatom writes: `SPEED x=N`, as the reply to `SPEED` and unsolicited on every
 * change, so the launcher can say so on screen. */
#define SPEED_MAX 4
/* What a fast turn leaves spare, for the save tick, the socket and the pad. */
#define FAST_MARGIN_US 1500.0
/* Presenting's cost before the first present of a game has measured it: the
 * exit summary's figure for stretch, 6.6 to 7.0 ms, rounded up. */
#define FAST_PRESENT_GUESS_US 7000.0
static int  g_speed = 1;

/* For a core that asks (GET_FASTFORWARDING, env.c) */
bool diatom_fast_forwarding(void)
{
	return g_speed > 1;
}

/* A Super Game Boy's clock is the SNES's, 2.4% faster than a Game Boy's, and
 * mGBA, running an SGB-flagged game as one (in the GBC folder, where the model
 * is left to autodetect), announces the SGB's frame rate and the plain Game
 * Boy's sound rate, and sends sound at the SGB's: 2.4% more than it says,
 * which rate control's 0.5% cannot cover. On the Brick, 2026-10-27, 85995
 * frames dropped in 89 s, heard as crackle (TortOS#60). Both rates are the
 * clock over a fixed count, 70224 cycles a frame and 32 a sample, so the rate
 * it really sends is known: the SGB's clock over 32. It applies only while
 * mGBA announces the Game Boy's rate, so a later mGBA that announces the right
 * one is left alone.
 *
 * Measured over whole games in Donkey Kong it came out 0.02-0.11% higher, from
 * frames a game makes longer or shorter by turning its screen off and on, well
 * inside rate control's reach.
 *
 * 2.1.0 measured every core's rate half a second into the game instead, and
 * a fresh start of Metroid Zero Mission, whose 23rd frame carries 600 sound
 * frames where the rest carry 1097, read 1.5% slow and dropped sound for ten
 * seconds until the next check (TortOS#73). A known constant for the one core
 * and mode that needs it, and nothing measured for the rest. */
#define GB_CLOCK  4194304.0   /* cycles a second */
#define SGB_CLOCK 4295454.0

static double sound_rate(const struct retro_system_info *si,
                         const struct retro_system_av_info *av)
{
	if (si->library_name && !strcmp(si->library_name, "mGBA") &&
	    av->timing.sample_rate == GB_CLOCK / 32.0 &&
	    fabs(av->timing.fps - SGB_CLOCK / 70224.0) < 0.001)
		return SGB_CLOCK / 32.0;
	return av->timing.sample_rate;
}

/* One step's measurement: frames run against the pacing periods that passed,
 * so a step that hit the budget says how far short it fell. Wall time
 * includes the menu; the speed does not. */
static void step_report(int step, long frames, long periods, uint64_t held_us,
                        uint64_t dropped)
{
	printf("diatom: speed %dx held %.1fs, reached %.2fx, %llu audio frame(s) dropped\n",
	       step, held_us / 1000000.0, periods ? (double)frames / (double)periods : 0.0,
	       (unsigned long long)dropped);
	fflush(stdout);
}

static void speed_emit(void)
{
	if (diatom_proto_connected()) diatom_proto_send("SPEED\tx=%d", g_speed);
}

static bool take_pause_request(void)
{
	bool v = g_pause_requested;
	g_pause_requested = false;
	return v;
}

static bool menu_pause(const diatom_session *sn)
{
	/* The pause preview goes out BEFORE PAUSED, so by the time the launcher
	 * hears it owns the display, the frame it will dim and draw its menu over
	 * is already on disk. The order is the contract. */
	if (sn->preview && write_preview(sn->preview))
		diatom_proto_send("PREVIEW\tpath=%s", sn->preview);

	/* The same handover as EXIT, and the same requirement: nothing in flight
	 * before the launcher starts drawing. This was a 20ms sleep, which is a
	 * guess at how long a pan takes rather than a wait for one - right often
	 * enough to look correct and wrong whenever the panel or the load
	 * disagreed.
	 *
	 * KEEP, because the launcher is about to draw its menu over the frame the
	 * player stopped on. Parking the same picture that is already showing is
	 * invisible, which is why this handover never flashed. */
	diatom_port_present_stop(DIATOM_PARK_KEEP);
	g_paused_preview = sn->preview;

	/* Symmetrical with RUNNING: the launcher may draw from here. */
	diatom_proto_send("PAUSED");

	/* NOT for(;;). A paused Diatom blocks here on a socket with no timeout,
	 * and this loop used to ignore g_terminate entirely - so SIGTERM set the
	 * flag and nothing read it, and the process could not be signaled out of
	 * an open menu at all. Seen on the device 2026-08-29: the launcher was
	 * asked to quit, left its menu loop without sending RESUME, and went back
	 * to waiting on a game that would never report anything, while this sat
	 * waiting for a message that would never come. Each on the other.
	 *
	 * Ending the pause is enough. The caller stops the game, which is what a
	 * termination means anyway. */
	while (!g_terminate) {
		diatom_msg m;

		switch (diatom_proto_poll(&m, -1, false)) {
		case DIATOM_MSG_RESUME:
			/* The launcher had the display and therefore owned the levels;
			 * it may have moved either while its menu was up. Re-read rather
			 * than step from a cached value nobody is at. */
			diatom_levels_invalidate();
			levels_forget();
			/* And whatever was pressed in it is the launcher's, already
			 * acted on: dropped here, not read back on the next poll. */
			diatom_port_input_reset();
			/* The menu was time somebody spent pressing buttons, on a pad
			 * this loop could not see. Counting it as idle is the opposite of
			 * what happened. */
			idle_restart();
			diatom_proto_send("RUNNING");
			return true;
		case DIATOM_MSG_STOP:
			return false;
		case DIATOM_MSG_QUIT:
			g_quit_requested = true;
			return false;
		case DIATOM_MSG_SAVE:
			diatom_proto_send(diatom_state_save(g_core, m.path)
			                  ? "SAVED\tpath=%s" : "ERROR\tcode=save_failed\tmsg=%s",
			                  m.path);
			break;
		case DIATOM_MSG_LOAD:
			diatom_proto_send(diatom_state_load(g_core, m.path)
			                  ? "LOADED\tpath=%s" : "ERROR\tcode=state_rejected\tmsg=%s",
			                  m.path);
			break;
		case DIATOM_MSG_RESET:
			/* The menu's Reset row. The game stays paused - the launcher
			 * still owns the display and sends RESUME when it is done. */
			g_core->reset();
			/* Same reason as a state load, one step further: the game is
			 * back at the title screen and nothing in progress survived. */
			diatom_cheevos_runtime_reset();
			diatom_proto_send("RESETDONE");
			break;
		case DIATOM_MSG_OPTIONS:
			diatom_options_emit();
			break;
		case DIATOM_MSG_SETOPT:
			diatom_proto_send(diatom_options_set(m.key, m.value)
			                  ? "OPTSET\tkey=%s" : "ERROR\tcode=bad_option\tmsg=%s",
			                  m.key);
			break;
		case DIATOM_MSG_HANGUP:
			/* The launcher died while holding the menu open. Resuming is the
			 * kinder failure: the alternative strands the player in a paused
			 * game with nothing left to talk to. */
			diatom_port_log(DIATOM_LOG_WARN,
			                "launcher vanished during menu; resuming the game");
			return true;
		/* The whole state plane, by falling through rather than by a list.
		 * This enumerated five of the seven, so DISPLAY and SETDISPLAY sent
		 * from an open menu parsed correctly, reached here, and were dropped
		 * with no reply and no log line - silence indistinguishable from the
		 * launcher never having sent them. Display mode was then the one
		 * state-plane setting unreachable at the one moment a launcher has a
		 * menu open to reach it from, which the state plane's design says it
		 * must not be.
		 *
		 * The running loop always dispatched this way and so could not go
		 * stale; a hand-kept list here would go stale again the next time
		 * something joins the plane. state_plane_msg returns false for
		 * anything it does not own, so this discards exactly what the old
		 * default discarded. */
		default:
			state_plane_msg(&m);
			break;
		}
	}

	/* Terminated while paused. False means "do not resume", and the caller's
	 * loop then ends the session the same way STOP does - which is the right
	 * reading of a termination arriving with the menu open. */
	return false;
}

/* One game, start to finish. Extracted so the protocol loop can run it
 * repeatedly in a process that never exits - which is what makes a warm
 * launch ~35 ms instead of ~700 ms, measured. Standalone mode calls it once.
 *
 * Returns 0 on a clean run, or a non-zero code that main turns into an exit
 * status or an ERROR message depending on how Diatom was started. */

/* ---- the state plane -------------------------------------------------- */

static const char *const level_kind_name[DIATOM_LEVEL_COUNT] = {
	"volume", "brightness"
};
static int g_last_index[DIATOM_LEVEL_COUNT];
static int g_last_count[DIATOM_LEVEL_COUNT];

static void levels_forget(void)
{
	int k;
	for (k = 0; k < DIATOM_LEVEL_COUNT; k++) g_last_index[k] = g_last_count[k] = -1;
}

static void level_emit(int k, int idx, int cnt)
{
	g_last_index[k] = idx;
	g_last_count[k] = cnt;
	diatom_proto_send("LEVEL\tkind=%s\tindex=%d\tcount=%d",
	                  level_kind_name[k], idx, cnt);
}

/* Polled with the input bitfield and emitted on change. The port cannot send
 * anything itself - it must not know the protocol exists - so this is the
 * whole path by which a volume press during a game reaches a launcher that is
 * not drawing and cannot see it. */
static void levels_tick(void)
{
	int k, idx, cnt;

	if (!diatom_proto_connected()) return;
	for (k = 0; k < DIATOM_LEVEL_COUNT; k++) {
		if (!diatom_levels_get((diatom_level_kind)k, &idx, &cnt)) continue;
		if (idx == g_last_index[k] && cnt == g_last_count[k]) continue;
		level_emit(k, idx, cnt);
	}
}

/* Did any level change since the last call? Separate from levels_tick, which
 * runs only with a launcher connected: the MENU chord it serves matters
 * standalone too, where MENU's release ends the session. */
static bool levels_moved(void)
{
	static int seen[DIATOM_LEVEL_COUNT] = { -1, -1 };
	bool moved = false;
	int k, idx, cnt;

	for (k = 0; k < DIATOM_LEVEL_COUNT; k++) {
		if (!diatom_levels_get((diatom_level_kind)k, &idx, &cnt)) continue;
		if (seen[k] >= 0 && idx != seen[k]) moved = true;
		seen[k] = idx;
	}
	return moved;
}

static void levels_emit_all(void)
{
	int k, idx, cnt, n = 0;

	for (k = 0; k < DIATOM_LEVEL_COUNT; k++)
		if (diatom_levels_get((diatom_level_kind)k, &idx, &cnt)) n++;

	/* count=0 is the answer on a port with no level control of its own, and
	 * says "expect no events" instead of leaving it to be inferred. */
	diatom_proto_send("LEVELS\tcount=%d", n);
	for (k = 0; k < DIATOM_LEVEL_COUNT; k++)
		if (diatom_levels_get((diatom_level_kind)k, &idx, &cnt))
			level_emit(k, idx, cnt);
}

/* ---- audio output ------------------------------------------------------- */

/* What was last reported, so a change can be told from a restatement. Empty is
 * a real value here - the default device - so `said` carries "reported at all"
 * rather than overloading the empty string with it. */
static char g_audio_said[128];
static bool g_audio_ever;

/* Restated on the next tick rather than assumed to be still known. Called at
 * game start beside levels_forget, for the same reason: a launcher that has
 * just connected, or just launched, should be told where sound is going
 * without having to ask. Unlike the levels there is no invalidate to pair with
 * it - the port's sink can only be moved through SETAUDIO, which Diatom
 * handles itself, so it cannot change behind this process's back. */
static void audio_forget(void)
{
	g_audio_ever = false;
	g_audio_said[0] = '\0';
}

static void audio_emit(void)
{
	char dev[128];

	diatom_port_audio_get(dev, sizeof dev);
	snprintf(g_audio_said, sizeof g_audio_said, "%s", dev);
	g_audio_ever = true;
	diatom_proto_send("AUDIO\tdevice=%s", dev);
}

/* Polled beside the levels, and for the identical reason: the port cannot send
 * anything itself, so this is the whole path by which a sink that died under
 * it - a headset switched off mid-game - reaches a launcher that is not
 * drawing and cannot see it. */
static void audio_tick(void)
{
	char dev[128];

	if (!diatom_proto_connected()) return;
	diatom_port_audio_get(dev, sizeof dev);
	if (g_audio_ever && !strcmp(dev, g_audio_said)) return;
	audio_emit();
}

/* No ERROR branch, deliberately. A device that will not open is not a refusal:
 * the port falls back, keeps the game running, and AUDIO then says where the
 * sound actually ended up. Answering with an error as well would describe the
 * same event twice and invite a launcher to treat it as fatal, which is the
 * behavior the fallback exists to remove. */
static void audio_set(const diatom_msg *m)
{
	char actual[128];

	diatom_port_audio_set(m->device, actual, sizeof actual);
	audio_emit();
}

/* ---- mute ----------------------------------------------------------------
 *
 * The one state on the plane this host does not own. The launcher reads a
 * hardware switch; all this side does is hold the output off and, crucially,
 * stop putting it back. See include/diatom_port.h. */
static void mute_emit(void)
{
	diatom_proto_send("MUTE\ton=%d", diatom_port_mute_get() ? 1 : 0);
}

static void mute_set(const diatom_msg *m)
{
	diatom_port_mute_set(m->on != 0);
	mute_emit();
}

/* ---- quiet ---------------------------------------------------------------
 *
 * The launcher decides - it knows what else is playing - and this host does
 * it, in its own stream (audio.c). Never emitted unsolicited: nothing on this
 * side ever changes it. */
static void quiet_emit(void)
{
	diatom_proto_send("QUIET\ton=%d", diatom_audio_quiet_get() ? 1 : 0);
}

static void quiet_set(const diatom_msg *m)
{
	diatom_audio_quiet(m->on != 0);
	quiet_emit();
}

/* ---- low battery ----------------------------------------------------------
 *
 * The launcher decides what counts as low and says so; the port draws the
 * disc over the game. Held across sessions like quiet, and never emitted
 * unsolicited: nothing on this side ever changes it. */
static bool g_low_battery, g_low_battery_blink;

static void low_battery_emit(void)
{
	diatom_proto_send("LOWBATTERY\ton=%d\tblink=%d", g_low_battery ? 1 : 0,
	                  g_low_battery_blink ? 1 : 0);
}

static void low_battery_set(const diatom_msg *m)
{
	g_low_battery = m->on != 0;
	g_low_battery_blink = g_low_battery && m->blink != 0;
	diatom_port_low_battery(g_low_battery, g_low_battery_blink);
	low_battery_emit();
}

/* ---- discs ---------------------------------------------------------------
 *
 * Which disc of a multi-disc game is in the tray, and changing it. The game
 * says when - "insert disc 2" on its own screen - and the player answers from
 * the launcher's menu, so the launcher asks DISC to know whether to offer the
 * row at all and sends SETDISC when it is chosen.
 *
 * The tray is the core's (env.c takes it from SET_DISK_CONTROL_*), and only a
 * game that is loaded has discs: idle, or a core with no tray, answers
 * `count=0`, which a launcher reads as "no disc row", the same as a game on
 * one disc answering `count=1`. */
static bool disc_ready(void)
{
	return g_core && g_core->game_loaded && g_core->disc_set &&
	       g_core->disc.get_num_images && g_core->disc.get_image_index;
}

static void disc_emit(void)
{
	const struct retro_disk_control_ext_callback *d;
	unsigned idx, n;
	char label[128] = "";
	bool ejected;
	size_t i;

	if (!disc_ready()) {
		diatom_proto_send("DISC	count=0");
		return;
	}
	d = &g_core->disc;
	n = d->get_num_images();
	idx = d->get_image_index();
	ejected = d->get_eject_state && d->get_eject_state();
	/* The core's own name for the disc: the file's stem for an .m3u entry,
	 * "Metal Gear Solid (USA) (Disc 1)" style, or what a PBP calls its
	 * discs. Only the EXT interface has labels; without one the launcher
	 * counts them itself. Tabs and newlines would break the line protocol. */
	if (d->get_image_label && idx < n &&
	    d->get_image_label(idx, label, sizeof label)) {
		for (i = 0; label[i]; i++)
			if (label[i] == '\t' || label[i] == '\n' || label[i] == '\r')
				label[i] = ' ';
	} else {
		label[0] = '\0';
	}
	diatom_proto_send("DISC\tindex=%u\tcount=%u\tejected=%d%s%s", idx, n,
	                  ejected ? 1 : 0, label[0] ? "\tlabel=" : "", label);
}

/* The whole swap in one message: open the tray, change the disc, close it.
 * The three steps are libretro's, and a launcher sending them one by one
 * would leave the tray open for as long as its menu took - with
 * pcsx_rearmed the game sees the lid open and then, two seconds after the
 * close, a new disc, which is what a PlayStation's own lid looked like.
 *
 * Answered with DISC either way, as SETMAP is, so the launcher always knows
 * which disc is in. A refused index changes nothing. */
static void disc_set(const diatom_msg *m)
{
	const struct retro_disk_control_ext_callback *d;
	unsigned n;
	bool ok = false;

	if (disc_ready() && g_core->disc.set_image_index) {
		d = &g_core->disc;
		n = d->get_num_images();
		if (m->index >= 0 && (unsigned)m->index < n) {
			if (d->set_eject_state) d->set_eject_state(true);
			ok = d->set_image_index((unsigned)m->index);
			/* Closed again even when the change failed, so a refusal
			 * leaves the game with a disc in rather than an open lid. */
			if (d->set_eject_state) d->set_eject_state(false);
		}
	}
	if (!ok)
		diatom_proto_send("ERROR\tcode=bad_disc\tmsg=%d", m->index);
	else {
		char msg[64];
		snprintf(msg, sizeof msg, "disc: changed to %d", m->index);
		diatom_port_log(DIATOM_LOG_INFO, msg);
	}
	disc_emit();
}

/* ---- channels ------------------------------------------------------------
 *
 * Which ears hear what, for a player who hears with one. The launcher decides
 * per output and this host mixes, in its own stream (audio.c), like quiet. */
static void channels_emit(void)
{
	diatom_proto_send("CHANNELS\tmode=%s",
	                  diatom_channels_names[diatom_audio_channels_get()]);
}

static void channels_set(const diatom_msg *m)
{
	int c;

	for (c = 0; c < DIATOM_CHANNELS_COUNT; c++)
		if (!strcmp(diatom_channels_names[c], m->dmode)) break;
	/* A name it does not know changes nothing, and says so: guessing stereo
	 * for it would undo a mix the player depends on. Answered either way,
	 * as SETMAP is, so the launcher always knows what is in force. */
	if (c == DIATOM_CHANNELS_COUNT)
		diatom_proto_send("ERROR\tcode=bad_channels\tmsg=%s", m->dmode);
	else
		diatom_audio_channels((diatom_channels)c);
	channels_emit();
}

static void level_set(const diatom_msg *m)
{
	int k, idx, cnt;

	for (k = 0; k < DIATOM_LEVEL_COUNT; k++)
		if (!strcmp(level_kind_name[k], m->lkind)) break;

	if (k == DIATOM_LEVEL_COUNT || m->count <= 0 ||
	    !diatom_levels_set((diatom_level_kind)k, m->index, m->count)) {
		diatom_proto_send("ERROR\tcode=bad_level\tmsg=%s", m->lkind);
		return;
	}
	/* Answer in OUR positions rather than echoing theirs. The launcher sent a
	 * fraction of its own ladder and needs to know which rung it landed on. */
	if (diatom_levels_get((diatom_level_kind)k, &idx, &cnt))
		level_emit(k, idx, cnt);
}

static void display_set(const diatom_msg *m)
{
	int mode = g_mode, i;
	diatom_filter filter = g_filter;

	if (m->dmode[0]) {
		for (i = 0; i < diatom_mode_count; i++)
			if (!strcmp(diatom_modes[i].name, m->dmode)) break;
		if (i == diatom_mode_count) {
			diatom_proto_send("ERROR\tcode=bad_display\tmsg=%s", m->dmode);
			return;
		}
		mode = i;
	}
	if (m->dfilter[0]) {
		if      (!strcmp(m->dfilter, "nearest")) filter = DIATOM_FILTER_NEAREST;
		else if (!strcmp(m->dfilter, "sharp"))   filter = DIATOM_FILTER_SHARP;
		else {
			diatom_proto_send("ERROR\tcode=bad_display\tmsg=%s", m->dfilter);
			return;
		}
	}
	/* Both fields validated before either is applied, for the same reason
	 * SETMAP is all-or-nothing: a half-applied setting is one nobody asked
	 * for and neither side believes in. */
	apply_display(mode, filter);
}

/* ---- two screens ---------------------------------------------------------
 *
 * The layout is the launcher's to choose and Diatom's to apply, as the display
 * mode is: SETSCREENS writes it, SCREENS reads it, and both answer with the
 * whole state, `dual=` saying whether this game is one it applies to. The
 * stylus is the player's, toggled in the game with L2, so STYLUS is also sent
 * unsolicited on every change; the launcher's menu reads it to say On or Off. */
/* Paused, the launcher's menu is drawn over the preview in DISPLAY's rect:
 * a new layout is a new picture there as well as a new rect, so it is written
 * again and said, between the DISPLAY that apply_display sent and the
 * SCREENS or STYLUS that closes the answer. */
static void repreview(void)
{
	if (g_paused_preview && write_preview(g_paused_preview))
		diatom_proto_send("PREVIEW\tpath=%s", g_paused_preview);
}

static void screens_emit(void)
{
	diatom_proto_send("SCREENS\tlayout=%s\tswap=%d\tdual=%d",
	                  scr_layout_names[g_layout], g_swap ? 1 : 0, g_dual ? 1 : 0);
}

static void screens_set(const diatom_msg *m)
{
	int l = g_layout;

	if (m->layout[0] && (l = scr_layout_index(m->layout)) < 0) {
		diatom_proto_send("ERROR\tcode=bad_screens\tmsg=%s", m->layout);
		return;
	}
	if (m->swap[0] && strcmp(m->swap, "0") && strcmp(m->swap, "1")) {
		diatom_proto_send("ERROR\tcode=bad_screens\tmsg=swap=%s", m->swap);
		return;
	}
	/* Both checked before either applies, as SETDISPLAY does. */
	g_layout = (scr_layout)l;
	if (m->swap[0]) g_swap = m->swap[0] == '1';
	if (g_dual) {
		apply_display(g_mode, g_filter);
		repreview();
	}
	screens_emit();
}

static void stylus_emit(void)
{
	if (diatom_proto_connected())
		diatom_proto_send("STYLUS\ton=%d", sty_on() ? 1 : 0);
}

/* On only for a game with two screens: anywhere else there is no bottom
 * screen to touch, and the answer says so - on=0 - rather than agreeing to a
 * stylus nothing will draw. Focus and single put the touched screen first,
 * so they are laid out again. */
static void stylus_set(bool on)
{
	if (!g_dual) on = false;
	if (on != sty_on()) {
		sty_set(on);
		printf("diatom: stylus %s\n", on ? "on" : "off");
		fflush(stdout);
		if (g_dual && g_layout != SCR_STACKED) {
			apply_display(g_mode, g_filter);
			repreview();
		}
	}
	stylus_emit();
}

/* Socket mode's premap, on its own thread. A signal may land on this thread
 * too, which the wake pipe above already allows for.
 *
 * It waits for the launcher's PREMAP, which says it has drawn its first
 * frame. Mapped the moment Diatom listened, the cores were read from the card
 * at the same moment as the launcher's GPU libraries and firmware, and the
 * launcher's video setup took 510-630 ms instead of 260: its first frame came
 * about 0.4 s later on most boots of the TrimUI Brick (measured 2026-10-18,
 * six cold boots, and three with the mapping moved after the first frame: all
 * fast). A RUN meanwhile maps its own core, as a RUN always could.
 *
 * Five seconds of idle at most, for a launcher that never says: then the
 * cores are mapped anyway. Idle only, because a game resumed at power-on
 * comes before the launcher draws anything, and mapping beside it would read
 * the card under a running game; the launcher's first frame after it sends
 * PREMAP. Counted in 20 ms naps rather than against a clock: the wall clock
 * can jump (a Brick boots into 2076 and NTP puts it back fifty years), and a
 * condition on the monotonic clock is not on every system this builds for. A
 * nap's lateness after PREMAP is nothing beside the mapping. */
#define PREMAP_WAIT_MS 5000

static volatile sig_atomic_t g_premap_go;

static void premap_go(void) { g_premap_go = 1; }

static void *premap_cores(void *dir)
{
	struct timespec nap = { 0, 20 * 1000000L };
	int n, waited;

	for (waited = 0; !g_premap_go && waited < PREMAP_WAIT_MS; ) {
		nanosleep(&nap, NULL);
		if (g_phase == PHASE_IDLE) waited += 20;
	}

	n = diatom_core_premap(dir);
	fprintf(stderr, "diatom: premapped %d core(s) from %s\n", n, (const char *)dir);
	return NULL;
}

/* Shared by all three message loops. A launcher may read or write any of this
 * whenever it likes: the two moments it is drawing - menu and idle - are
 * exactly the moments Diatom is not, and it is no less valid mid-game. */
static bool state_plane_msg(const diatom_msg *m)
{
	switch (m->kind) {
	case DIATOM_MSG_INPUTS: diatom_input_emit_labels(); return true;
	case DIATOM_MSG_MAP:    diatom_input_emit_map();    return true;
	case DIATOM_MSG_SETMAP:
		if (!diatom_input_set_map(m->map))
			diatom_proto_send("ERROR\tcode=bad_map\tmsg=%s", m->map);
		/* Answered either way, so a refusal cannot leave the launcher
		 * believing a map it does not have. */
		diatom_input_emit_map();
		return true;
	case DIATOM_MSG_LEVELS:   levels_emit_all(); return true;
	case DIATOM_MSG_SETLEVEL: level_set(m);      return true;
	case DIATOM_MSG_AUDIO:    audio_emit();      return true;
	case DIATOM_MSG_SETAUDIO: audio_set(m);      return true;
	case DIATOM_MSG_MUTE:     mute_emit();       return true;
	case DIATOM_MSG_SETMUTE:  mute_set(m);       return true;
	case DIATOM_MSG_QUIET:    quiet_emit();      return true;
	case DIATOM_MSG_SETQUIET: quiet_set(m);      return true;
	case DIATOM_MSG_LOWBATTERY:    low_battery_emit(); return true;
	case DIATOM_MSG_SETLOWBATTERY: low_battery_set(m); return true;
	case DIATOM_MSG_CHANNELS:    channels_emit();  return true;
	case DIATOM_MSG_SETCHANNELS: channels_set(m);  return true;
	case DIATOM_MSG_DISC:    disc_emit();  return true;
	case DIATOM_MSG_SETDISC: disc_set(m);  return true;
	case DIATOM_MSG_SCREENS:    screens_emit();      return true;
	case DIATOM_MSG_SETSCREENS: screens_set(m);      return true;
	case DIATOM_MSG_STYLUS:     stylus_emit();       return true;
	case DIATOM_MSG_SETSTYLUS:  stylus_set(m->on);   return true;
	case DIATOM_MSG_PREMAP:
		/* The launcher has drawn: the cores and the sound device, which both
		 * waited so as not to compete with its start. */
		premap_go();
		diatom_port_audio_start();
		return true;
	case DIATOM_MSG_SPEED:    speed_emit();      return true;
	case DIATOM_MSG_DISPLAY:
		/* apply_display is what emits, so ask it to restate the current one
		 * rather than growing a second path that could disagree with it. */
		apply_display(g_mode, g_filter);
		return true;
	case DIATOM_MSG_SETDISPLAY: display_set(m); return true;
	case DIATOM_MSG_OVERLAY:
		/* Not on the state plane's query/write shape, because there is no
		 * state to query: it is a thing that happens and then stops. The
		 * launcher is told whether the image was usable, and nothing else. */
		diatom_proto_send(show_overlay(m->path, (unsigned)m->count)
		                  ? "OVERLAID" : "ERROR\tcode=bad_overlay\tmsg=%s",
		                  m->path);
		return true;
	case DIATOM_MSG_CHEEVOS: diatom_cheevos_emit(); return true;
	case DIATOM_MSG_SETCHEEVOS:
		if (m->console) diatom_cheevos_set_console((unsigned)m->console);
		diatom_cheevos_load(m->path);
		diatom_cheevos_emit();
		return true;
	default: return false;
	}
}

/* One present, with the stylus cursor in it while it is on. Stamped into the
 * kept frame and taken out again straight after, because every port copies
 * the frame inside present (diatom_port.h): the cursor reaches the panel and
 * the frame kept for dupes and previews never holds it. With the cursor up the
 * frame goes out even when the core sent none new, so a cursor moved over a
 * still picture moves on the panel too. */
static void present_frame(bool fresh)
{
	int  bpp = g_policy.pixfmt == DIATOM_PIX_RGB565 ? 2 : 4;
	bool cur = g_dual && sty_on() && g_frame && g_frame_w > 0;

	if (diatom_hw_active()) {
		if (fresh) diatom_port_present_gpu(g_frame_w, g_frame_h, g_dst, g_filter);
		return;
	}
	if (cur) sty_stamp(g_frame, g_frame_pitch, bpp, g_frame_w, g_frame_h);
	diatom_port_present((fresh || cur) ? g_frame : NULL,
	                    g_frame_w, g_frame_h, g_frame_pitch,
	                    g_policy.pixfmt, g_dst, g_filter);
	if (cur) sty_unstamp(g_frame, g_frame_pitch, bpp);
}

/* The clock ceiling a game runs under: the system's (RUN power=), stepped
 * toward full for the rest of the game at the first sign it is short of
 * clock - a frame that starts a whole period late, a stall, sound dropped -
 * since the ceiling is there to save battery, never frames. Not again before
 * g_power_calm_us: a clock change holds the memory up for a moment, and the
 * first seconds of a game (its warmup, a state loaded) run late anyway. */
static int      g_power_level;
static uint64_t g_power_calm_us;

static void power_trouble(const char *why, uint64_t now)
{
	if (g_power_level == DIATOM_POWER_FULL || now < g_power_calm_us) return;
	g_power_level--;
	diatom_port_power(g_power_level);
	g_power_calm_us = now + 1000000;
	printf("diatom: power up to %s after %s\n",
	       g_power_level == DIATOM_POWER_FULL ? "full" : "medium", why);
}

static int run_session_inner(const diatom_session *sn)
{
	struct retro_system_av_info av;
	struct retro_system_info si;
	double   frame_us, next_us;
	uint64_t t_start, paused_us = 0;   /* menu time, excluded from the rate */
	uint32_t buttons = 0, prev_buttons = 0, held_at_entry = 0;
	uint32_t chord_hold = 0;    /* R1, while it belongs to MENU+R1 */
	bool     menu_armed = false, menu_chorded = false;
	long     fast_frames = 0, fast_turns = 0, fast_unshown = 0;
	/* frameskip=late: frames run late and left undrawn, and how many in a
	 * row the last of them was. */
	long     late_unshown = 0;
	int      late_row = 0;
	bool     last_shown = true;
	/* What retro_run cost in all, drawn and undrawn apart, for the summary:
	 * the paced rate says only that the loop kept up, this says by how much -
	 * the one number that compares a container with a device. */
	double   run_us_shown = 0.0, run_us_unshown = 0.0;
	long     run_n_shown = 0, run_n_unshown = 0;
	/* What a core frame costs, run to be shown and run to be skipped, kept
	 * apart because skipping the drawing is the point (see the batch below).
	 * For a core that doesn't skip they come out the same. */
	double   shown_us = 0.0, unshown_us = 0.0;
	/* The step in force, measured: what it reached, how long it was held, and
	 * any audio dropped while it was - logged when it changes and at exit. */
	long     step_frames = 0, step_periods = 0;
	uint64_t step_t0 = 0, step_drop0 = 0;
	/* Sound dropped at 1x so far, and whether the last turn ran fast, for
	 * the power ceiling (power_trouble) */
	uint64_t power_drops = 0;
	bool     power_fast = false;
	long     locked_at = 0;     /* frame the current rect was computed on */
	/* The sound rate the resampler uses: the announced one, or what the core
	 * really sends where that is known to differ (sound_rate). */
	double   audio_rate = 0.0;

	/* Every game starts from identity and from an unknown level, so a launcher
	 * that sends no map gets no map, and the first poll reports where the
	 * levels actually are without being asked. */
	diatom_input_reset_map();
	levels_forget();
	audio_forget();
	diatom_levels_invalidate();
	long frames = 0, geom_changes = 0, resyncs = 0;
	size_t q_min = (size_t)-1, q_max = 0;
	bool stop = false;

	/* Per-game state that must not carry over from the previous session. */
	g_speed = 1;
	g_audio_fast = false;
	g_present_frames = 0;
	g_present_us = 0;
	g_frame_w = g_frame_h = 0;
	g_frame_pitch = 0;
	g_frame_fresh = false;
	/* One screen until this game's geometry says it can be two. */
	g_dual = false;
	g_nplanes = 0;
	diatom_port_planes(NULL, 0);
	sty_reset(0, 0);
	diatom_env_pointer(false, 0, 0);

	g_core = diatom_core_resident(sn->core);
	if (!g_core) {
		diatom_proto_send("ERROR\tcode=core_missing\tmsg=%s", sn->core);
		return 3;
	}
	diatom_env_bind(g_core, &g_policy, &g_caps);

	g_core->get_system_info(&si);
	printf("diatom: %s %s\n",
	       si.library_name ? si.library_name : "?",
	       si.library_version ? si.library_version : "?");

	/* Most cores declare their options during core open, but not all: some
	 * wait until content is loaded, because what they offer depends on the
	 * ROM. So list after loading when a ROM was given, and before when it was
	 * not - listing what a core offers should not *require* owning a game. */
	if (sn->list_only && !sn->rom) {
		diatom_options_list();
		return 0;
	}

	if (!sn->rom && !g_policy.supports_no_game) {
		fprintf(stderr, "diatom: this core needs content; pass --rom\n");
		diatom_proto_send("ERROR\tcode=rom_unreadable\tmsg=core needs content");
		return 4;
	}
	/* Firmware, checked BEFORE the core is asked to load.
	 *
	 * Diatom does not know that a PC Engine CD needs `syscard3.pce`, and will
	 * not learn: it has no core list, and a firmware table is a core list by
	 * another name. The launcher already maps system to core, so it is the
	 * thing that knows, and it says so with `firmware=`.
	 *
	 * Checked here rather than after a failed load because the distinction
	 * the protocol draws is about the DISPLAY. A missing System Card means
	 * the game never started, so the launcher must keep drawing and hear
	 * ERROR. The core only ever tells us `retro_load_game` returned false,
	 * which is the same answer it gives for a corrupt ROM. */
	if (sn->firmware && *sn->firmware) {
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", g_policy.system_dir, sn->firmware);
		if (access(path, R_OK) != 0) {
			fprintf(stderr, "diatom: missing firmware %s\n", path);
			diatom_proto_send("ERROR\tcode=bios_missing\tmsg=%s", sn->firmware);
			return 4;
		}
	}

	/* Before the core starts, because the address space is resolved during
	 * the load and it cannot be laid out until someone says which console it
	 * belongs to. Diatom never guesses this. */
	diatom_cheevos_set_console((unsigned)sn->console);

	if (!diatom_core_start(g_core, sn->rom)) {
		/* Say the firmware was found, so a launcher that reports this does not
		 * send someone hunting for a BIOS they already have. */
		if (sn->firmware && *sn->firmware)
			diatom_proto_send("ERROR\tcode=rom_unreadable"
			                  "\tmsg=core refused the rom (%s was present)",
			                  sn->firmware);
		else
			diatom_proto_send("ERROR\tcode=rom_unreadable\tmsg=core refused the rom");
		return 4;
	}

	if (sn->list_only) {
		diatom_options_list();
		diatom_core_stop(g_core);
		return 0;
	}

	/* THE PLAIN JOYPAD, WHICH IS NOT THE THREE-BUTTON PAD. This comment used
	 * to say it was, and ZyperOS's backlog carried a whole item built on that
	 * reading: Genesis six-button games were supposed to be playing with three
	 * because of this line.
	 *
	 * They are not. Read out of genesis_plus_gx on 2026-09-17, after Eric
	 * pressed L1 in a fighter and it threw a punch: RETRO_DEVICE_JOYPAD is a
	 * case in its retro_set_controller_port_device, and it sets padtype to
	 * DEVICE_PAD2B | DEVICE_PAD6B | DEVICE_PAD3B - every pad at once, which is
	 * also what config_default() starts with. The core decides per game from
	 * there, and it puts Genesis X, Y and Z on L1, X and R1.
	 *
	 * So this is "give me the core's ordinary pad", not a choice between pad
	 * types, and it is the right call for every core here. A core that wants a
	 * specific device type needs the controller_type field in diatom_policy,
	 * which is still unused, and a launcher that knows to ask. Digital-only
	 * input removes axes, not device types. */
	g_core->set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

	/* SRAM first: it is the game's own data and is not optional. A state, if
	 * the launcher asked for one, is layered on top - and a state that fails
	 * to load is not an error, it just means the game starts normally. That
	 * fallback is what makes resume-by-default safe across a core update. */
	diatom_save_init(g_core, g_policy.save_dir, sn->rom);
	if (sn->state_load && !diatom_state_load(g_core, sn->state_load))
		printf("diatom: no usable state at %s; starting the game normally\n",
		       sn->state_load);

	/* Standalone's install. Socket mode did this in terminate_init before it
	 * ever listened, and re-doing it here is idempotent; standalone has no
	 * such moment, because there is nothing before the game. */
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_terminate;
		sigaction(SIGTERM, &sa, NULL);
		sigaction(SIGINT,  &sa, NULL);
		sigaction(SIGHUP,  &sa, NULL);
	}

	g_core->get_system_av_info(&av);
	printf("diatom: %ux%u (max %ux%u) aspect %.4f, %.4f fps, %.0f Hz -> %d Hz\n",
	       av.geometry.base_width, av.geometry.base_height,
	       av.geometry.max_width, av.geometry.max_height,
	       (double)av.geometry.aspect_ratio,
	       av.timing.fps, av.timing.sample_rate, g_caps.audio_rate);

	/* The device opens here if the launcher has not had it opened yet: a
	 * game resumed at power-on comes before the launcher draws. Capacity 0
	 * when it will not open, which turns rate control off. */
	audio_rate = sound_rate(&si, &av);
	if (audio_rate != av.timing.sample_rate)
		printf("diatom: Super Game Boy timing: resampling from %.0f Hz, "
		       "not the %.0f Hz announced\n", audio_rate, av.timing.sample_rate);
	{
		int cap = diatom_port_audio_start() ? g_caps.audio_buffer_frames : 0;

		diatom_audio_configure(audio_rate, g_caps.audio_rate, cap);
		g_foreign_high = (size_t)cap * 3 / 4;
	}
	diatom_audio_prime();

	/* The rect is computed from BASE geometry and does not move again unless
	 * the user asks.
	 *
	 * Cores announce hires by calling SET_GEOMETRY with a larger base_width
	 * mid-run (measured: 3 of 6 do this). Recomputing an integer factor from
	 * the new width collapses 3x to 1x and the picture shrinks to a fifth.
	 * Holding the rect fixed keeps the picture the same size AND is more
	 * faithful: SNES and PC Engine hires pixels are physically half-width, so
	 * 512 columns belong in the same screen width as 256.
	 *
	 * A mode change recomputes from these same base values, never from the
	 * current geometry, which is what keeps the invariant intact. */
	g_base_w      = (int)av.geometry.base_width;
	g_base_h      = (int)av.geometry.base_height;
	g_base_aspect = (double)av.geometry.aspect_ratio;
	/* A vertical arcade game: the rect is for the turned picture. */
	diatom_env_rotate_dims(&g_base_w, &g_base_h);
	if (diatom_env_rotation())
		printf("diatom: turned %u quarter(s) counter-clockwise to %dx%d\n",
		       diatom_env_rotation(), g_base_w, g_base_h);
	/* Two screens, if the launcher said so and the frame can be cut in two.
	 * A DS frame is 256x384 and stays that, so the base geometry is the
	 * frame's for the whole game. */
	if (sn->screens == 2 && g_base_h >= 2 && g_base_h % 2 == 0) {
		g_dual = true;
		sty_reset(g_base_w, g_base_h / 2);
		printf("diatom: two screens of %dx%d, layout %s%s\n", g_base_w, g_base_h / 2,
		       scr_layout_names[g_layout], g_swap ? ", bottom first" : "");
		if (sn->stylus) sty_set(true);
	} else if (sn->screens == 2) {
		printf("diatom: screens=2, but %dx%d does not cut in two; one picture\n",
		       g_base_w, g_base_h);
	}
	apply_display(sn->mode, sn->filter);
	if (g_dual) { screens_emit(); stylus_emit(); }
	locked_at = 0;

	/* Pace against a monotonic clock at the core's own rate, on an ABSOLUTE
	 * schedule kept in floating point.
	 *
	 * Absolute matters: an incremental "sleep frame_us each time" accumulates
	 * every scheduler overshoot forever, while a running deadline absorbs them -
	 * a long sleep is followed by a correspondingly short one.
	 *
	 * Floating point matters too, if less: 1000000/59.7275 is 16742.63us, and
	 * truncating loses 38ms per hour.
	 *
	 * Audio drift is not this loop's problem. It is handled by rate control,
	 * because no panel and no core will ever agree on a rate. */
	frame_us = 1000000.0 / (av.timing.fps > 0 ? av.timing.fps : 60.0);

	/* Idle reporting belongs to this game, and this game has not asked yet.
	 * The launcher's SETIDLE comes behind SETLEVEL, SETMUTE and SETDISPLAY,
	 * and the loop reads one message a frame, so the first frames checked the
	 * LAST game's timeout against the last game's last press. Anyone who had
	 * sat on the shelf longer than Auto Off had the device switch off seven
	 * frames into the next game - measured on the device 2026-09-28, and seen
	 * by a reviewer before that. Off until SETIDLE turns it back on. */
	g_idle_after_ms = 0;
	idle_restart();

	/* The display handover, and the reason the protocol separates ERROR from
	 * EXIT: from here the launcher must stop drawing. Announced before the
	 * warmup, because the warmup already puts frames on the panel. */
	diatom_proto_send("RUNNING");
	g_phase = PHASE_RUNNING;   /* a crash from here on is EXIT, not ERROR */

	/* A set sent with RUN is watched from the first frame. A launcher that is
	 * still fetching one sends SETCHEEVOS when it arrives; both paths run the
	 * same loader. */
	if (sn->cheevos) diatom_cheevos_load(sn->cheevos);

	/* Warm up before starting the clock. The first frames create the texture,
	 * fault in code paths and prime the audio device; measured on a COLD
	 * process, they overrun the frame budget badly enough to trip a resync
	 * every single run. Timing them reports a rate the loop never actually
	 * sustains - and, worse, hides whether the steady-state loop is correct.
	 *
	 * Note for anyone tempted to skip this when resident: measured 2026-08-25
	 * across five launches in one process, the warmup costs 23-34 ms every
	 * time, flat. It is not cold-start overhead that residency has already
	 * paid; it is three real frames of emulation and blitting at ~8 ms each.
	 * Skipping it would save frames, not overhead. */
	/* Input already held when the game begins is not the game's. Pressing A on
	 * the shelf to launch otherwise fires a weapon in the game: the launcher's
	 * A is still down when the first frame runs, `prev_buttons` starts at 0 so
	 * it reads as a fresh edge, and `cb_input_state` returns the level.
	 *
	 * Same fault as the menu resume below, at the sibling entry point that fix
	 * did not cover, and its reasoning transfers word for word - including
	 * suppressing until genuine RELEASE rather than narrowing to what went down
	 * recently, since the player may well have released A on the shelf and
	 * pressed it again to launch.
	 *
	 * BEFORE the warmup rather than after it. The warmup calls `retro_run`
	 * three times and the core polls the pad from inside it, so a mask set
	 * after would leak on exactly the frames it exists to protect.
	 *
	 * This is also the only thing that clears a mask left over from the
	 * previous game. `g_suppress` is static and the resident process never
	 * resets it, so quitting with SELECT held used to carry that suppression
	 * into the next game's warmup frames. */
	diatom_port_input_poll();
	prev_buttons  = diatom_port_input_state();
	held_at_entry = prev_buttons;
	diatom_env_suppress(held_at_entry);

	/* The system's clock ceiling, before the warmup, so its frames already
	 * run under it */
	g_power_level = sn->power;
	diatom_port_power(g_power_level);
	g_power_calm_us = diatom_port_now_us() + 2000000;
	if (g_power_level != DIATOM_POWER_FULL)
		printf("diatom: power %s\n", g_power_level == DIATOM_POWER_LIGHT ? "light" : "medium");

	/* From here the core's DEBUG/INFO goes nowhere. Load-time chatter was worth
	 * keeping - it is where a core states its version - but per-frame chatter is
	 * not, and under the launcher it is written to the SD card. The measurement
	 * is in env.c; the short version is that mGBA's per-DMA logging cost 16 fps
	 * and put a real hole in the audio. */
	diatom_env_core_log_quiet(true);

	{
		uint64_t w0 = diatom_port_now_us();
		int w;
		g_audio_warmup = true;
		for (w = 0; w < 3; w++) {
			g_core->run();
			if (diatom_hw_active())
				present_frame(g_frame_fresh);
			else
				diatom_port_present(g_frame, g_frame_w, g_frame_h, g_frame_pitch,
				                    g_policy.pixfmt, g_dst,
				                    g_filter);
		}
		g_audio_warmup = false;
		printf("diatom: warmup 3 frames in %.1f ms\n",
		       (diatom_port_now_us() - w0) / 1000.0);
	}

	t_start = diatom_port_now_us();
	next_us = (double)t_start;
	step_t0 = t_start;
	step_drop0 = diatom_audio_dropped();

	while (!diatom_port_should_quit() && !g_terminate && !stop &&
	       (sn->limit <= 0 || frames < sn->limit)) {
		uint64_t now;

		/* At 1x a turn of this loop is one core frame and one pacing period.
		 * Fast, a turn covers TWO periods and is drawn once: presenting costs
		 * ~7 ms on this thread, so drawing every refresh would leave the core
		 * too little of each. It runs core frames up to the step - two periods'
		 * worth - or until the next one would not fit in the two periods less
		 * what presenting costs, whichever comes first. The present cost is the
		 * running average, so a cheaper display mode buys speed by itself. */
		const bool fast = g_speed > 1;
		const int periods = fast ? 2 : 1;
		/* Fast, at the top clock; see diatom_port_fast_clock. Every ceiling
		 * lifted first, since a minimum held at the top cannot pass the
		 * maximum, and put back after, with time for the clocks to settle. */
		if (fast && !power_fast) diatom_port_power(DIATOM_POWER_FULL);
		diatom_port_fast_clock(fast);
		if (!fast && power_fast) {
			diatom_port_power(g_power_level);
			g_power_calm_us = diatom_port_now_us() + 1000000;
		}
		if (!fast && diatom_audio_dropped() != power_drops)
			power_trouble("dropped sound", diatom_port_now_us());
		power_fast = fast;
		power_drops = diatom_audio_dropped();
		const uint64_t batch_t0 = diatom_port_now_us();
		const double budget = periods * frame_us - FAST_MARGIN_US -
		                      (g_present_frames ? (double)g_present_us /
		                                          (double)g_present_frames
		                                        : FAST_PRESENT_GUESS_US);
		int ran = 0;

		g_frame_fresh = false;
		g_audio_fast = fast;
		/* Only the last frame of a turn is drawn on the panel, so fast, a core
		 * with a frameskip runs the frames before it unshown and skips their
		 * drawing (env.c, diatom_env_frame_shown). A frame runs unshown while
		 * the turn has room after it for one more unshown and the shown one to
		 * end on; otherwise it is the shown one, and the turn ends with it. So
		 * the frame on the panel is always one the core drew, never a guess
		 * that was wrong.
		 *
		 * A core without a frameskip keeps the rule from before it: frames
		 * until the next would not fit, judged after each one. The first try
		 * gave both kinds of core the new rule, with each cost timed inside
		 * retro_run alone, and that left out the step's other work: it fitted
		 * one frame too many now and then, and the overrun turn took a third
		 * refresh. Sonic at 4x on the Pixel 2 reached 2.05x against 2.24x
		 * before, measured 2026-10-27. Each cost is now the whole step's, on
		 * the wall clock, as the old rule's always was. */
		const bool skips = fast && diatom_env_frame_skippable();
		for (;;) {
			bool     shown = true;
			uint64_t s0 = diatom_port_now_us();
			double   cost;

			if (!fast && (double)s0 > next_us + frame_us)
				power_trouble("a late frame", s0);

			if (skips && ran + 1 < periods * g_speed &&
			    !(sn->limit > 0 && frames + 1 >= sn->limit))
				shown = (double)(s0 - batch_t0) + unshown_us + shown_us > budget;
			/* frameskip=late, at 1x: a frame that starts more than half a
			 * period behind the schedule is run with its drawing left out,
			 * so the game keeps its speed and the panel its last picture.
			 * Two in a row at most, so at least every third frame is drawn
			 * however far behind the loop is - 20 pictures a second at the
			 * worst, with the game, its sound and its timing whole. A core
			 * that draws in its own time (a DS game's 3D on an A35) gets the
			 * time back from the frames it does not draw; the schedule's own
			 * debt rule below still drops a stall of four frames. The idea,
			 * at 1x and from libretro's audio buffer status callback, is
			 * nelynes's slotPlus, as plorpOS's Diatom has it too. */
			else if (!fast && sn->late_skip && diatom_env_frame_skippable()) {
				shown = !((double)s0 > next_us + frame_us * 0.5 && late_row < 2);
				if (shown) late_row = 0;
				else { late_row++; late_unshown++; }
			}
			diatom_env_frame_shown(shown);
			last_shown = shown;

			g_core->run();
			frames++;
			ran++;

			/* Immediately after the frame the core produced, and before
			 * anything else can touch memory. This one line is why
			 * achievements belong to the frontend: conditions compare against
			 * the previous frame, so they have to be evaluated once per frame,
			 * here, and not over a socket at 10Hz. Every frame the game runs,
			 * fast or not. */
			diatom_cheevos_frame();

			/* A running average, an eighth new, of each kind. The first one
			 * measured seeds both, so the first fast turn has a figure. */
			cost = (double)(diatom_port_now_us() - s0);
			if (shown) { run_us_shown += cost; run_n_shown++; }
			else       { run_us_unshown += cost; run_n_unshown++; }
			if (shown) shown_us = shown_us > 0.0 ? shown_us + (cost - shown_us) / 8.0 : cost;
			else       unshown_us = unshown_us + (cost - unshown_us) / 8.0;
			if (unshown_us <= 0.0) unshown_us = shown_us;

			if (skips) {
				if (!shown) fast_unshown++;
				if (shown) break;
			} else {
				double spent;

				if (!fast || ran >= periods * g_speed) break;
				if (sn->limit > 0 && frames >= sn->limit) break;
				spent = (double)(diatom_port_now_us() - batch_t0);
				if (spent + spent / ran > budget) break;
			}
		}
		step_frames += ran;
		step_periods += periods;

		if (fast) {
			/* Silence for the time this turn stands for, so the queue holds
			 * its target and the rate control never sees fast forward. At the
			 * rate the resampler is using, which is not always the announced
			 * one. */
			static const int16_t quiet[2 * 4096];
			size_t n = (size_t)(periods * audio_rate / av.timing.fps);

			if (n > 4096) n = 4096;
			diatom_audio_push(quiet, n);
			fast_frames += ran;
			fast_turns++;
		}

		/* The rect was locked at load and never moved. That is right when the
		 * load-time geometry is the mode the game runs in and the change is an
		 * excursion - SNES and PC Engine hires - and wrong when the load-time
		 * value is a boot artifact.
		 *
		 * Measured 2026-08-26 with envlog, a probe, over 3600 frames:
		 *
		 *   genesis_plus_gx  Herzog Zwei       256x192 for 29 frames, then
		 *                                      320x224 for the other 3571
		 *                    Phantasy Star IV  256x192 for ONE frame
		 *   snes9x2010       four games        256x224 for all 3600
		 *   mednafen_pce_fast three games      256x243 for all 3600
		 *
		 * So the discriminator is not what changed, it is whether what we
		 * locked onto ever really held: relock only if the geometry we are
		 * displaying turned out to be transient. Genesis corrects itself
		 * within half a second, during the boot logo. A hires menu opened
		 * minutes in does not qualify and the rect stays put, which is the
		 * behavior locking the rect exists to protect. */
		if (diatom_env_geometry_changed()) {
			int nw, nh;
			double na;

			geom_changes++;
			if (frames - locked_at < SETTLE_FRAMES &&
			    diatom_env_new_geometry(&nw, &nh, &na) &&
			    (nw != g_base_w || nh != g_base_h)) {
				printf("diatom: geometry settled %dx%d -> %dx%d "
				       "at frame %ld\n",
				       g_base_w, g_base_h, nw, nh, frames);
				/* Reprinted in the SAME format as the load-time
				 * line so anything parsing that format sees the
				 * mode the game actually runs in by taking the
				 * last one. The core facts script took the first and
				 * recorded Genesis as 256x192 at 1.5238 - twenty-
				 * nine frames of boot - which a display
				 * table was then computed from. */
				printf("diatom: %dx%d (max %ux%u) aspect %.4f, "
				       "%.4f fps, %.0f Hz -> %d Hz\n",
				       nw, nh, av.geometry.max_width,
				       av.geometry.max_height, na,
				       av.timing.fps, av.timing.sample_rate,
				       g_caps.audio_rate);
				g_base_w      = nw;
				g_base_h      = nh;
				g_base_aspect = na;
				apply_display(g_mode, g_filter);
				locked_at = frames;
			}
		}

		{
			uint64_t p0 = diatom_port_now_us();
			/* A frame run undrawn is not presented: its pixels are the last
			 * drawn frame's, already on the panel, and presenting them again
			 * would spend on the upload what skipping saved. */
			present_frame(g_frame_fresh && last_shown);
			g_present_us += diatom_port_now_us() - p0;
			g_present_frames++;
		}

		diatom_save_tick();

		if (diatom_proto_active()) {
			diatom_msg m;
			switch (diatom_proto_poll(&m, 0, true)) {
			case DIATOM_MSG_STOP: stop = true; break;
			case DIATOM_MSG_QUIT: stop = true; g_quit_requested = true; break;
			/* Consumed at the top of the next frame, beside the MENU press it
			 * stands in for - not here, because the menu takes the display and
			 * this is the middle of a frame that has already been presented. */
			case DIATOM_MSG_PAUSE: g_pause_requested = true; break;
			case DIATOM_MSG_SETIDLE:
				g_idle_after_ms = (unsigned)(m.count > 0 ? m.count : 0);
				g_idle_since_us = diatom_port_now_us();
				g_idle_said = false;
				break;
			case DIATOM_MSG_OPTIONS: diatom_options_emit(); break;
			case DIATOM_MSG_SETOPT:
				diatom_proto_send(diatom_options_set(m.key, m.value)
				                  ? "OPTSET\tkey=%s"
				                  : "ERROR\tcode=bad_option\tmsg=%s", m.key);
				break;
			/* Also honored while running, not only from the menu. Slot CHOICE
			 * belongs in the launcher's menu, but nothing about that requires
			 * the game to be paused to write a state. */
			case DIATOM_MSG_SAVE:
				diatom_proto_send(diatom_state_save(g_core, m.path)
				                  ? "SAVED\tpath=%s"
				                  : "ERROR\tcode=save_failed\tmsg=%s", m.path);
				break;
			case DIATOM_MSG_LOAD:
				diatom_proto_send(diatom_state_load(g_core, m.path)
				                  ? "LOADED\tpath=%s"
				                  : "ERROR\tcode=state_rejected\tmsg=%s", m.path);
				break;
			case DIATOM_MSG_RESET:
				g_core->reset();
				/* Same reason as a state load, one step further: the game
				 * is back at the title screen and nothing in progress
				 * survived. */
				diatom_cheevos_runtime_reset();
				diatom_proto_send("RESETDONE");
				break;
			/* HANGUP is deliberately not a stop. Diatom is a separate process
			 * so a launcher that died cannot take a running game with it. */
			default: state_plane_msg(&m); break;
			}
		}

		/* The level keys, stepped here from what the port reports, before the
		 * tick that reports a move and the MENU chord check that reads one. */
		diatom_levels_frame();
		levels_tick();
		audio_tick();

		buttons = diatom_port_input_state();
		idle_note_input(buttons != 0);

		/* Latched at whichever entry point began these frames - game start or
		 * menu resume - and narrowed here: a button the player has genuinely
		 * let go of becomes the game's again on its next real press. Tested
		 * against the raw pad state, so a button that is suppressed but still
		 * physically down stays latched. */
		held_at_entry &= buttons;

		/* R1 belongs to MENU+R1 from the moment MENU is down - not from when
		 * R1 is, so the frame R1 goes down cannot leak into the game - and
		 * stays hidden after MENU is let go, until R1 is too. */
		if (buttons & DIATOM_BIT(DIATOM_BTN_MENU))
			chord_hold = DIATOM_BIT(DIATOM_BTN_R1) |
			             (g_dual ? DIATOM_BIT(DIATOM_BTN_L1) : 0);
		else
			chord_hold &= buttons;

		/* The stylus, for a game with two screens. L2 turns stylus mode on
		 * and off; while on, the d-pad moves the cursor and R2 is the stylus
		 * on the screen. L2 and R2 are the stylus's whenever there are two
		 * screens - a DS has neither button, so no game loses one - and the
		 * d-pad is the cursor's only while the mode is on. The pointer goes
		 * to the core on its next frame. */
		{
			uint32_t sty_mask = 0;

			if (g_dual) {
				uint32_t down = buttons & ~prev_buttons;

				sty_mask = DIATOM_BIT(DIATOM_BTN_L2) | DIATOM_BIT(DIATOM_BTN_R2);
				if ((down & DIATOM_BIT(DIATOM_BTN_L2)) &&
				    !(buttons & DIATOM_BIT(DIATOM_BTN_MENU)))
					stylus_set(!sty_on());
				if (sty_on()) {
					unsigned dirs =
					    (buttons & DIATOM_BIT(DIATOM_BTN_UP)    ? STY_UP    : 0) |
					    (buttons & DIATOM_BIT(DIATOM_BTN_DOWN)  ? STY_DOWN  : 0) |
					    (buttons & DIATOM_BIT(DIATOM_BTN_LEFT)  ? STY_LEFT  : 0) |
					    (buttons & DIATOM_BIT(DIATOM_BTN_RIGHT) ? STY_RIGHT : 0);

					sty_frame(dirs, (buttons & DIATOM_BIT(DIATOM_BTN_R2)) != 0);
					sty_mask |= DIATOM_BIT(DIATOM_BTN_UP) | DIATOM_BIT(DIATOM_BTN_DOWN) |
					            DIATOM_BIT(DIATOM_BTN_LEFT) | DIATOM_BIT(DIATOM_BTN_RIGHT);
				}
				{
					int16_t px, py;
					bool pd = sty_pointer(&px, &py);

					diatom_env_pointer(pd, px, py);
				}
			}

			/* The single writer. Every reason to hide a button from the core
			 * is composed here, so no reason can clear another. */
			diatom_env_suppress(held_at_entry | chord_hold | sty_mask);
		}

		/* MENU+R1 steps the speed. MENU is a modifier, so it is armed by its
		 * press and acts on its RELEASE. */
		{
			uint32_t pressed = buttons & ~prev_buttons;

			if (pressed & DIATOM_BIT(DIATOM_BTN_MENU)) {
				menu_armed = true;
				menu_chorded = false;
			}
			/* A level that moves while MENU is held made MENU a modifier
			 * too: the Pixel 2's brightness is MENU plus the volume keys,
			 * and letting go of MENU afterwards must not open the menu.
			 * Read every frame so `levels_moved` stays current. */
			if (levels_moved() && (buttons & DIATOM_BIT(DIATOM_BTN_MENU)))
				menu_chorded = true;

			if ((buttons & DIATOM_BIT(DIATOM_BTN_MENU)) &&
			    (pressed & DIATOM_BIT(DIATOM_BTN_R1))) {
				step_report(g_speed, step_frames, step_periods,
				            diatom_port_now_us() - step_t0,
				            diatom_audio_dropped() - step_drop0);
				g_speed = g_speed % SPEED_MAX + 1;
				menu_chorded = true;
				printf("diatom: speed %dx\n", g_speed);
				fflush(stdout);
				speed_emit();
				step_frames = step_periods = 0;
				step_t0 = diatom_port_now_us();
				step_drop0 = diatom_audio_dropped();
			}
			/* MENU+L1 swaps the two screens, a DS game's: which one is on
			 * top, large, or shown, without opening the menu. */
			if (g_dual && (buttons & DIATOM_BIT(DIATOM_BTN_MENU)) &&
			    (pressed & DIATOM_BIT(DIATOM_BTN_L1))) {
				g_swap = !g_swap;
				menu_chorded = true;
				apply_display(g_mode, g_filter);
				screens_emit();
			}
		}

		/* MENU is Diatom's own key and the ports no longer act on it, because
		 * what it means is host policy: standalone it ends the session, under
		 * the launcher it opens the launcher's menu. On release, and only for a
		 * press this loop saw, unless that press became a chord: a MENU still
		 * held when a menu resumes the game was not pressed here. */
		if ((menu_armed && !menu_chorded &&
		     ((prev_buttons & ~buttons) & DIATOM_BIT(DIATOM_BTN_MENU))) ||
		    take_pause_request()) {
			menu_armed = false;
			if (!diatom_proto_active()) {
				stop = true;
			} else {
				uint64_t pause_t0 = diatom_port_now_us();
				bool resumed;

				/* The menu is the launcher's, at the governor's clock and
				 * under no ceiling; a fast game takes the top one back on
				 * its next turn, and a game at 1x its ceiling here. */
				diatom_port_fast_clock(false);
				diatom_port_power(DIATOM_POWER_FULL);
				resumed = menu_pause(sn);
				diatom_port_power(power_fast ? DIATOM_POWER_FULL : g_power_level);
				g_power_calm_us = diatom_port_now_us() + 1000000;
				power_drops = diatom_audio_dropped();
				g_paused_preview = NULL;

				/* The summary line divides frames by wall clock, and an open
				 * menu produces no frames, so without this a long menu reads
				 * as the core having run slow. Measured on device: "364
				 * frames in 25.20s = 14.44 fps" was six seconds of play and
				 * nineteen of menu. Pacing already rebases next_us below for
				 * the same reason; the summary was simply never told. */
				paused_us += diatom_port_now_us() - pause_t0;

				if (!resumed) {
					stop = true;
				} else {
					/* The clock ran on while the menu was open. Without this the
					 * loop believes it is thousands of frames late and spends the
					 * next second catching up. */
					next_us = (double)diatom_port_now_us();

					/* And the device played the sound buffer out meanwhile, so
					 * it is filled back to target before the first frame. */
					diatom_audio_prime();

					/* Re-read input and treat it as already-seen, so a key still
					 * held is not a fresh press.
					 *
					 * Clearing prev_buttons instead looks equivalent and is not:
					 * a finger can still be on a key when the launcher resumes,
					 * and the next frame reads that as a fresh press. Measured
					 * with a human 2026-08-25, when the menu opened on MENU's
					 * press - one press produced two menus. On release now, a
					 * MENU held across the resume would otherwise arm, and open
					 * the menu again when let go. */
					diatom_port_input_poll();
					prev_buttons = diatom_port_input_state();
					menu_armed = false;

					/* Resuming fast is the one moment the speed could surprise,
					 * so it is said again. At 1x there is nothing to say. */
					if (g_speed > 1) speed_emit();

					/* prev_buttons covers everything edge-triggered, which is
					 * every consumer except the one that matters: the core reads
					 * the LEVEL, in cb_input_state. So choosing Continue with A
					 * put an A into the game - a stray jump on every dismissal.
					 *
					 * menu_pause blocks in proto_poll and never polls the pad, so
					 * the whole menu session's events queue up and land in one
					 * drain here. Nothing in that batch was aimed at the game.
					 *
					 * Suppressed until RELEASE rather than until the next press,
					 * and deliberately not narrowed to buttons that went down
					 * while paused. That narrowing looks more precise and has a
					 * hole: release A during the menu, press it again to choose
					 * Continue, and it was held before the menu too, so it would
					 * not be caught. Holding a direction across a menu is the cost,
					 * and it self-corrects on the next press.
					 *
					 * Set here as well as at the top of the loop because `continue`
					 * goes straight to retro_run - recording the latch without
					 * applying it would leak the button on exactly the frame this
					 * exists to protect. */
					held_at_entry = prev_buttons;
					diatom_env_suppress(held_at_entry);
					continue;
				}
			}
		}
		prev_buttons = buttons;

		{
			size_t q = diatom_port_audio_queued();
			if (q < q_min) q_min = q;
			if (q > q_max) q_max = q;
		}
		diatom_audio_sync();

		next_us += frame_us * periods;
		now = diatom_port_now_us();

		if (next_us > (double)now) {
			struct timespec ts;
			double d = next_us - (double)now;
			ts.tv_sec  = (time_t)(d / 1000000.0);
			ts.tv_nsec = (long)(fmod(d, 1000000.0) * 1000.0);
			nanosleep(&ts, NULL);
		} else if ((double)now - next_us > frame_us * 4.0) {
			/* More than four frames behind. Something stalled - the scheduler,
			 * a page fault, a slow core - and trying to catch up would just run
			 * fast for a while, which looks worse than dropping the debt.
			 * Counted, because a loop that resyncs often is a loop that is
			 * lying about its frame rate. */
			next_us = (double)now;
			resyncs++;
			if (g_speed <= 1) power_trouble("a stall", now);
		}
		/* Otherwise keep the debt and let the next short sleep repay it. */
	}

	/* Read the clock before the capture: converting and writing a full-screen
	 * BMP costs hundreds of milliseconds, and it happens after the last frame.
	 * Measured on the Brick: leaving it inside the timed span understated a
	 * perfectly paced 59.73fps loop as 58.1 - a measurement bug wearing the
	 * costume of a pacing bug. */
	if (g_terminate)
		printf("diatom: terminated by signal; saving\n");
	diatom_port_fast_clock(false);
	diatom_port_power(DIATOM_POWER_FULL);

	/* Both paths, and in this order: the game's own save first, because losing
	 * it is a defect, then the state, which is a convenience. */
	diatom_save_shutdown();
	if (sn->state_exit) diatom_state_save(g_core, sn->state_exit);

	/* The frame the player was looking at, for the launcher's card - written
	 * with the exit state so a card that shows a preview always has a state
	 * to resume, and announced before EXIT so it is on disk before the
	 * launcher redraws. */
	if (sn->preview && write_preview(sn->preview))
		diatom_proto_send("PREVIEW\tpath=%s", sn->preview);

	if (fast_frames)
		step_report(g_speed, step_frames, step_periods,
		            diatom_port_now_us() - step_t0,
		            diatom_audio_dropped() - step_drop0);

	{
		uint64_t t_end = diatom_port_now_us();

		if (sn->shot)
			printf("diatom: capture %s: %s\n", sn->shot,
			       diatom_port_capture(sn->shot) ? "ok" : "FAILED");

		uint64_t span = t_end - t_start;
		double fast_secs = fast_turns * 2.0 * frame_us / 1000000.0;
		double secs = (span > paused_us ? span - paused_us : 0) / 1000000.0 - fast_secs;
		long   paced = frames - fast_frames;

		if (secs < 0) secs = 0;
		printf("diatom: %ld frames in %.2fs = %.2f fps (target %.4f)\n",
		       paced, secs, secs > 0 ? paced / secs : 0.0, av.timing.fps);
		/* Fast forward is kept out of the rate, as the menu is, and said. */
		if (fast_frames)
			printf("diatom: %ld more frames fast over %.2fs = %.2fx, "
			       "excluded from the rate above\n",
			       fast_frames, fast_secs,
			       fast_secs > 0 ? fast_frames / fast_secs / av.timing.fps : 0.0);
		if (run_n_shown)
			printf("diatom: the core took %.2f ms a drawn frame on average "
			       "(%.0f fps if unpaced)\n",
			       run_us_shown / run_n_shown / 1000.0,
			       1000000.0 / (run_us_shown / run_n_shown));
		if (run_n_unshown)
			printf("diatom: and %.2f ms a frame run undrawn, over %ld\n",
			       run_us_unshown / run_n_unshown / 1000.0, run_n_unshown);
		if (late_unshown)
			printf("diatom: %ld frame(s) ran late at 1x and were left undrawn "
			       "(frameskip=late), %.1f%% of them\n", late_unshown,
			       paced ? late_unshown * 100.0 / paced : 0.0);
		if (fast_unshown)
			printf("diatom: %ld of them run unshown, the core told to skip drawing them; "
			       "a frame %.1f ms shown, %.1f ms unshown\n",
			       fast_unshown, shown_us / 1000.0, unshown_us / 1000.0);
		/* Stated rather than silently subtracted, so the line cannot be read
		 * as wall clock by anyone who does not know it is not. */
		if (paused_us)
			printf("diatom: %.2fs paused, excluded from the rate above\n",
			       paused_us / 1000000.0);
		printf("diatom: %ld geometry change(s), last rect %dx%d at %d,%d\n",
		       geom_changes, g_dst.w, g_dst.h, g_dst.x, g_dst.y);
		printf("diatom: %ld resync(s)\n", resyncs);
		if (g_present_frames)
			printf("diatom: present avg %.2f ms over %ld frames\n",
			       (double)g_present_us / (double)g_present_frames / 1000.0,
			       g_present_frames);
		printf("diatom: audio dropped %llu frame(s)\n",
		       (unsigned long long)diatom_audio_dropped());
		printf("diatom: audio IN  peak %d rms %.0f, %llu of %llu non-zero\n",
		       diatom_audio_in_peak(), diatom_audio_in_rms(),
		       (unsigned long long)diatom_audio_in_nonzero(),
		       (unsigned long long)diatom_audio_in_samples());
		printf("diatom: audio OUT peak %d rms %.0f (%.1f%% fs), "
		       "%llu of %llu samples non-zero\n",
		       diatom_audio_peak(), diatom_audio_rms(), diatom_audio_peak() * 100.0 / 32767.0,
		       (unsigned long long)diatom_audio_nonzero(),
		       (unsigned long long)diatom_audio_samples());
		printf("diatom: audio quiet for %llu frame(s), fades included\n",
		       (unsigned long long)diatom_audio_quiet_frames());
		printf("diatom: audio queued min %zu max %zu final %zu, target %d, capacity %d\n",
		       q_min == (size_t)-1 ? 0 : q_min, q_max,
		       diatom_port_audio_queued(),
		       g_caps.audio_buffer_frames / 2, g_caps.audio_buffer_frames);
		printf("diatom: rate control drift %+.3f%% (max %+.3f%%)\n",
		       diatom_audio_ratio_drift() * 100.0, 0.5);
		diatom_env_core_log_summary();
	}

	diatom_core_stop(g_core);
	/* No dlclose. Ever. */

	/* Stop presenting BEFORE saying so. The launcher begins drawing the moment
	 * it reads EXIT, and until this call returned the flip thread could still
	 * have a pan in flight - two processes driving one framebuffer, which
	 * measured from the launcher side as a boundary sweeping down the panel
	 * over several frames. Not a wedge, because the window is milliseconds,
	 * but the contract below is only true once this has returned.
	 *
	 * BLANK, and the asymmetry with the pause above is the whole point. This
	 * park replaces whatever is on glass, and at an exit that is often the
	 * launcher's own in-game menu - the player quit from it. Parking the last
	 * frame there showed a bare game frame, without the menu, for the few tens
	 * of milliseconds before the shelf faded up: a flash on quit and never on
	 * opening the menu, which is exactly the asymmetry that identified it. The
	 * launcher fades up from black, so black is the state it is about to
	 * assume anyway. */
	diatom_port_present_stop(DIATOM_PARK_BLANK);
	/* The next game is one screen until it says otherwise; the planes go
	 * now, so nothing presented between games is cut in two. */
	g_dual = false;
	g_nplanes = 0;
	diatom_port_planes(NULL, 0);
	sty_reset(0, 0);
	diatom_env_pointer(false, 0, 0);

	/* The game ran and stopped, which is EXIT rather than ERROR whatever the
	 * reason. The launcher may take the display back now.
	 *
	 * Always `user`, and that is not a placeholder. Every way of arriving here
	 * is someone deciding to stop: STOP or QUIT from the launcher, MENU when
	 * standalone, a closed window, or SIGTERM - which on the Brick is the power
	 * button, so still the player. The other two reasons the protocol lists are
	 * produced elsewhere: `crash` by the handler above, since a crash never
	 * reaches this line, and `error` by nothing yet - there is no post-RUNNING
	 * failure Diatom can currently detect that is not a crash. */
	diatom_proto_send("EXIT\treason=user");
	return 0;
}

/* The phase belongs out here, not inside, so that an early return added later
 * cannot forget to clear it and leave the crash handler reporting on a game
 * that is no longer in flight. There are six such returns already. */
static int run_session(const diatom_session *sn)
{
	int rc;

	g_phase = PHASE_LOADING;
	rc = run_session_inner(sn);
	g_phase = PHASE_IDLE;
	return rc;
}

int main(int argc, char **argv)
{
	const char *core_path = NULL, *rom_path = NULL, *shot_path = NULL;

	g_host_thread = pthread_self();
	/* Stretch by default, judged on the panel. A handheld's screen
	 * is its whole interface, and full use of it beat both the letterbox and
	 * the crop. nearest because sharp earned nothing visible at these factors
	 * and is the only thing that has made this loop miss a frame. */
	const char *display = "stretch", *filter = "nearest";
	const char *state_load = NULL, *state_exit = NULL, *firmware = NULL, *tap = NULL;
	const char *preview_path = NULL, *cheevos_path = NULL;
	const char *sock = getenv("DIATOM_SOCKET");
	const char *cores_dir = NULL;
	const char *channels = "stereo";
	bool list_options = false, stylus = false;
	const char *frameskip = "";
	const char *power = "";
	int screens = 1;
	diatom_filter start_filter;
	long limit = 0;
	int i, start_mode = -1, console = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--core") && i + 1 < argc) core_path = argv[++i];
		else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_path = argv[++i];
		else if (!strcmp(argv[i], "--system") && i + 1 < argc) g_policy.system_dir = argv[++i];
		else if (!strcmp(argv[i], "--save") && i + 1 < argc) g_policy.save_dir = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else if (!strcmp(argv[i], "--display") && i + 1 < argc) display = argv[++i];
		else if (!strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
		else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) state_load = argv[++i];
		else if (!strcmp(argv[i], "--state-on-exit") && i + 1 < argc) state_exit = argv[++i];
		else if (!strcmp(argv[i], "--preview-on-exit") && i + 1 < argc) preview_path = argv[++i];
		else if (!strcmp(argv[i], "--firmware") && i + 1 < argc) firmware = argv[++i];
		/* Present on the command line as well as the protocol so a set can be
		 * exercised on the desktop with no launcher at all, which is how
		 * anything here gets debugged. */
		else if (!strcmp(argv[i], "--cheevos") && i + 1 < argc) cheevos_path = argv[++i];
		else if (!strcmp(argv[i], "--console") && i + 1 < argc)
			console = (int)strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--tap-audio") && i + 1 < argc) tap = argv[++i];
		/* The channels as SETCHANNELS sets them, for the launcher's standalone
		 * fallback: a player who hears with one ear should not lose half a
		 * game's sound because the resident one was not there. */
		else if (!strcmp(argv[i], "--channels") && i + 1 < argc) channels = argv[++i];
		else if (!strcmp(argv[i], "--socket") && i + 1 < argc) sock = argv[++i];
		else if (!strcmp(argv[i], "--cores") && i + 1 < argc) cores_dir = argv[++i];
		else if (!strcmp(argv[i], "--list-options")) list_options = true;
		/* RUN's screens=, frameskip= and SETSCREENS, for a DS game run
		 * standalone: the desktop and the container tests have no launcher. */
		else if (!strcmp(argv[i], "--screens") && i + 1 < argc)
			screens = (int)strtol(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--frameskip") && i + 1 < argc) frameskip = argv[++i];
		else if (!strcmp(argv[i], "--power") && i + 1 < argc) power = argv[++i];
		else if (!strcmp(argv[i], "--layout") && i + 1 < argc) {
			int l = scr_layout_index(argv[++i]);
			if (l < 0) { usage(); return 1; }
			g_layout = (scr_layout)l;
		}
		else if (!strcmp(argv[i], "--swap")) g_swap = true;
		else if (!strcmp(argv[i], "--stylus")) stylus = true;
		else if (!strcmp(argv[i], "--core-option") && i + 1 < argc) {
			/* Recorded now, applied when the core declares its options - the
			 * command line is parsed long before retro_set_environment runs. */
			char *kv = argv[++i], *eq = strchr(kv, '=');
			if (!eq) { fprintf(stderr, "diatom: --core-option wants key=value\n"); return 1; }
			*eq = '\0';
			diatom_options_set(kv, eq + 1);
			*eq = '=';
		}
		else { usage(); return 1; }
	}
	/* A core is required standalone, but under the protocol it arrives with
	 * each RUN, so its absence is not an error at startup. */
	if (!core_path && !sock) { usage(); return 1; }

	for (i = 0; i < diatom_mode_count; i++)
		if (!strcmp(display, diatom_modes[i].name)) start_mode = i;
	if (start_mode < 0) {
		fprintf(stderr, "diatom: unknown display mode '%s'\n", display);
		usage();
		return 1;
	}
	if (!strcmp(filter, "sharp"))        start_filter = DIATOM_FILTER_SHARP;
	else if (!strcmp(filter, "nearest")) start_filter = DIATOM_FILTER_NEAREST;
	else {
		fprintf(stderr, "diatom: unknown filter '%s'\n", filter);
		usage();
		return 1;
	}
	for (i = 0; i < DIATOM_CHANNELS_COUNT; i++)
		if (!strcmp(channels, diatom_channels_names[i])) break;
	if (i == DIATOM_CHANNELS_COUNT) {
		fprintf(stderr, "diatom: unknown channels '%s'\n", channels);
		usage();
		return 1;
	}
	diatom_audio_channels((diatom_channels)i);

	/* Reflect the process default in the live state, AFTER both are parsed.
	 *
	 * g_mode is a static and defaults to 0, which is diatom_modes[0] -
	 * `integer`. The real default is `stretch`, and it was only ever applied
	 * once a session started. Harmless while nothing could ask; the state
	 * plane lets a launcher query DISPLAY on an idle Diatom, and it answered
	 * `mode=integer` for a frontend that would have used `stretch`. Caught on
	 * hardware 2026-08-26 by asking with no game loaded. */
	g_mode   = start_mode;
	g_filter = start_filter;

	if (!g_policy.system_dir) g_policy.system_dir = ".";
	if (!g_policy.save_dir)   g_policy.save_dir   = ".";
	g_policy.pixfmt = DIATOM_PIX_RGB565;   /* libretro's default is 0RGB1555,
	                                          which Diatom refuses; every core
	                                          measured picked RGB565 anyway */

	/* Line-buffered, so a crash cannot swallow the log that explains it.
	 * Redirected to a file stdout is block-buffered, and the crash runs on
	 * 2026-08-25 lost everything since the last 4 KB boundary - including the
	 * warmup line, which made a session look like it had never got that far.
	 * Costs one write per line, and Diatom prints tens of lines per session,
	 * not per frame. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (!diatom_port_init(&g_caps)) {
		fprintf(stderr, "diatom: port init failed\n");
		return 2;
	}
	diatom_port_fast_clock(false);
	diatom_port_power(DIATOM_POWER_FULL);

	/* Once for the life of the process, not per session: the first thing a
	 * session does is dlopen and start a core, and a core that dies there is
	 * exactly the case worth reporting. */
	install_crash_handlers();

	if (tap) {
		char a[512], b[512];
		snprintf(a, sizeof a, "%s.in.raw",  tap);
		snprintf(b, sizeof b, "%s.out.raw", tap);
		diatom_audio_tap(a, b);
	}

	/* Two modes, and standalone is the primary one - designing for a program
	 * that stands alone is a stricter test than designing for one embedder.
	 *
	 * The socket mode is what makes launches fast: the process outlives the
	 * game, so SDL init and every core dlopen are paid once at boot rather
	 * than per launch. Measured: ~35 ms warm against 625-750 ms cold. */
	if (sock) {
		if (!diatom_proto_listen(sock)) return 5;
		/* AFTER listening, not before. Mapping first delayed the socket by
		 * the best part of 400ms, and the launcher probes early: it found no
		 * socket, fell back to running the game standalone, and every launch
		 * paid a dlopen the premap existed to avoid.
		 *
		 * And on a thread, not in line. A connection arriving during the
		 * mapping waited in the backlog, which was thought to cost the
		 * launcher nothing - but its connect waits for READY, and READY only
		 * went out once every core was mapped. Measured on the GKD Pixel 2
		 * 2026-10-02: the launcher's first frame waited about 280 ms on it,
		 * hidden until then behind a 350 ms input grace of its own.
		 *
		 * And the thread waits for the launcher's PREMAP: see premap_cores.
		 * Inline, with no thread, it maps at once, as waiting there would
		 * hold READY back for five seconds. */
		if (cores_dir) {
			pthread_t premap;

			if (pthread_create(&premap, NULL, premap_cores, (void *)cores_dir) == 0) {
				pthread_detach(premap);
			} else {
				premap_go();
				premap_cores((void *)cores_dir);
			}
		}
		terminate_init();

		while (!g_quit_requested && !g_terminate) {
			diatom_msg m;
			diatom_session sn;

			/* Idle: block. Nothing is on screen, so there is nothing to
			 * pace and no reason to spin. */
			switch (diatom_proto_poll(&m, -1, false)) {
			case DIATOM_MSG_RUN:     break;
			case DIATOM_MSG_OPTIONS: diatom_options_emit(); continue;
			case DIATOM_MSG_SETOPT:  diatom_options_set(m.key, m.value); continue;
			/* QUIT was falling through to `continue` here, so an IDLE Diatom
			 * ignored it and only a running one could be shut down - the exact
			 * opposite of what a launcher needs, since between games is when it
			 * would ask. Found 2026-08-25 while testing the crash paths: every
			 * run ended on the watchdog's SIGKILL rather than on QUIT. */
			case DIATOM_MSG_QUIT:    g_quit_requested = true; continue;
			default:                 state_plane_msg(&m); continue;
			}
			if (!m.core[0]) {
				diatom_proto_send("ERROR\tcode=core_missing\tmsg=no core given");
				continue;
			}

			memset(&sn, 0, sizeof sn);
			sn.core     = m.core;
			sn.rom      = m.rom[0] ? m.rom : NULL;
			sn.firmware = m.firmware[0] ? m.firmware : NULL;
			/* The launcher owns where things persist and says so
			 * per game. resume is load-if-exists - a missing state is a
			 * fresh start, not an error, which is what makes it safe to
			 * pass unconditionally. */
			sn.state_load = m.resume[0]     ? m.resume     : NULL;
			sn.state_exit = m.exit_state[0] ? m.exit_state : NULL;
			sn.preview    = m.preview[0]    ? m.preview    : NULL;
			sn.cheevos    = m.cheevos[0]    ? m.cheevos    : NULL;
			sn.console    = m.console;
			sn.screens    = m.screens;
			sn.late_skip  = !strcmp(m.frameskip, "late");
			sn.power      = !strcmp(m.power, "light")  ? DIATOM_POWER_LIGHT
			              : !strcmp(m.power, "medium") ? DIATOM_POWER_MEDIUM
			              : DIATOM_POWER_FULL;
			sn.mode   = start_mode;
			sn.filter = start_filter;
			run_session(&sn);   /* its own ERROR/EXIT is the report */

			/* The options the launcher sent were for THAT game. Cores are
			 * resident and several systems share one, so a value left
			 * standing reaches the next game through the same core. */
			diatom_options_clear_pending();
		}
		diatom_proto_close();
	} else {
		/* Named, not positional. A positional list mis-assigns silently the
		 * moment a field is added in the middle, and this one has grown
		 * three times. */
		diatom_session sn = {
			.core = core_path, .rom = rom_path, .shot = shot_path,
			.state_load = state_load, .state_exit = state_exit,
			.preview = preview_path, .firmware = firmware,
			.cheevos = cheevos_path, .console = console,
			.limit = limit, .mode = start_mode, .filter = start_filter,
			.list_only = list_options, .screens = screens,
			.late_skip = !strcmp(frameskip, "late"), .stylus = stylus,
			.power = !strcmp(power, "light")  ? DIATOM_POWER_LIGHT
			       : !strcmp(power, "medium") ? DIATOM_POWER_MEDIUM
			       : DIATOM_POWER_FULL,
		};
		int rc = run_session(&sn);
		if (rc) return rc;
	}
	diatom_audio_tap_close();
	diatom_port_shutdown();
	free(g_frame);
	return 0;
}
