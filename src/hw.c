/* Cores that draw with the GPU: libretro's hardware rendering, OpenGL ES only.
 *
 * Every core Diatom ran drew in software until the Dreamcast. Flycast draws
 * with OpenGL ES and has no software renderer worth playing, and the one that
 * runs Crazy Taxi 2 smoothly on the Pixel 2 is the 2021 libretro core (ROGUE
 * OS's), against about 21 fps for the standalone Flycast 2.7 here, whose
 * emulation thread sat at 99% of a core (measured 2026-11-04).
 *
 * The libretro half lives here; the port does the GL (diatom_port.h, GPU
 * frames). A core asks during retro_load_game (SET_HW_RENDER) and is answered
 * at once, but nothing is created until the load has succeeded: the frames'
 * size is the game's max geometry, which a core knows only then. Then the
 * context goes up on this thread, the core's context_reset runs, and the
 * frame loop presents GPU frames instead of pixels until the game is stopped.
 */
#include <stdio.h>
#include <string.h>

#include "diatom.h"

static struct retro_hw_render_callback g_hw;
static bool g_asked;            /* this load asked for the GPU */
static bool g_live;             /* the context is up and the core was told */

/* The core's own retro_run, while hw_run stands in for it */
static diatom_core *g_core;
static void       (*g_core_run)(void);

/* In place of the core's retro_run while its context is up, so every place
 * that runs a frame (the loop, the warmup, fast forward) does this too.
 *
 * Before the frame: its texture under the framebuffer. A core may read the
 * framebuffer's name once and never ask again (glsm does), so the turn to
 * the next texture has to come from here. */
static void hw_run(void)
{
	diatom_port_gpu_target();
	g_core_run();
}

static uintptr_t hw_framebuffer(void)
{
	return diatom_port_gpu_target();
}

static retro_proc_address_t hw_proc(const char *sym)
{
	return (retro_proc_address_t)diatom_port_gpu_proc(sym);
}

bool diatom_hw_request(struct retro_hw_render_callback *cb, const diatom_port_caps *caps)
{
	if (!caps || !caps->gpu) {
		printf("diatom: the core asked for the GPU, which this device's port does not draw\n");
		return false;
	}
	switch (cb->context_type) {
	case RETRO_HW_CONTEXT_OPENGLES2:
	case RETRO_HW_CONTEXT_OPENGLES3:
	case RETRO_HW_CONTEXT_OPENGLES_VERSION:
		break;
	default:
		/* OpenGL proper and Vulkan: none here. A core that asks for several
		 * in turn (Flycast does) comes back with the next. */
		return false;
	}
	cb->get_current_framebuffer = hw_framebuffer;
	cb->get_proc_address = hw_proc;
	g_hw = *cb;
	g_asked = true;
	return true;
}

void diatom_hw_load_begin(void)
{
	g_asked = false;
}

bool diatom_hw_start(diatom_core *c)
{
	struct retro_system_av_info av;
	diatom_gpu_spec spec;

	if (!g_asked) return true;
	memset(&av, 0, sizeof av);
	c->get_system_av_info(&av);
	spec.major   = g_hw.context_type == RETRO_HW_CONTEXT_OPENGLES2 ? 2
	             : g_hw.context_type == RETRO_HW_CONTEXT_OPENGLES3 ? 3
	             : (int)g_hw.version_major;
	spec.max_w   = (int)av.geometry.max_width;
	spec.max_h   = (int)av.geometry.max_height;
	spec.depth   = g_hw.depth;
	spec.stencil = g_hw.stencil;
	/* libretro's flag, true for GL's own way up (Flycast's), false for a
	 * core that draws its first row at the top */
	spec.top_down = !g_hw.bottom_left_origin;
	if (!diatom_port_gpu_begin(&spec)) {
		fprintf(stderr, "diatom: no GPU context for the core\n");
		return false;
	}
	g_live = true;
	g_core = c;
	g_core_run = c->run;
	c->run = hw_run;
	if (g_hw.context_reset) g_hw.context_reset();
	return true;
}

void diatom_hw_unload(void)
{
	/* After retro_unload_game, as RetroArch does it, and with the context
	 * still current. Before it, flycast2021 stopped drawing at once, its
	 * emulation thread waited on a frame that never came, and the unload
	 * waited on that thread: Quit hung (2026-11-04). */
	if (!g_live) return;
	if (g_hw.context_destroy) g_hw.context_destroy();
	diatom_port_gpu_end();
	g_core->run = g_core_run;
	g_core = NULL;
	g_live = false;
	g_asked = false;
}

bool diatom_hw_active(void)
{
	return g_live;
}
