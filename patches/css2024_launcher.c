// Counter-Strike: Source, last Mac build (October 2026 depot): launcher.dylib
// creates no game window in OpenGL mode.
//
// CSDLMgr::CreateHiddenGameWindow calls SDL_CreateWindow only when the command
// line has -vulkan; otherwise it uses a window nothing else creates, and the
// game stops with "Failed to create SDL window". -vulkan is no way out: it
// also makes the material system load shaderapivk, which the Mac build does
// not ship. This patch creates the window in OpenGL mode too, with
// SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN, so the OpenGL context made right
// after it works. The game itself also wants the depot's own launcher stub,
// which this build lacks; Half-Life 2's hl2_osx works.
#include "../src/patches.h"
#include <string.h>

static int apply(struct image *img)
{
	g32 fn = macho_local_sym(img, "__ZN7CSDLMgr22CreateHiddenGameWindowEPKcii");
	if (!fn) return 0;
	uint8_t *p = G2H(fn);
	static const uint8_t je[] = { 0x74, 0x39 };				// je: skip SDL_CreateWindow
	static const uint8_t flags[] = { 0xc7, 0x44, 0x24, 0x14, 0x08 };	// movl $SDL_WINDOW_HIDDEN, 0x14(%esp)
	if (memcmp(p + 0xc3, je, 2) || memcmp(p + 0xd3, flags, 5)) return 0;
	p[0xc3] = p[0xc4] = 0x90;			// always create the window
	p[0xd7] = 0x08 | 0x02;				// ... with SDL_WINDOW_OPENGL
	return 1;
}

const struct guest_patch patch_css2024_launcher = {
	.library = "launcher.dylib",
	.sha256 = "7e48beb8d372a111cf183e14b3df6ef17313eb988957dbc8ac0473a66a9a2f66",
	.game = "Counter-Strike: Source (last Mac build)",
	.what = "creates the OpenGL game window, which this build only made with -vulkan",
	.apply = apply,
};
