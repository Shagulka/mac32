// Applies the patches in patches/ when their library loads. A patch runs only
// on the exact build it names (SHA-256 of the i386 slice), so a fixed build
// from the vendor is left alone. MAC32_NOPATCH=1 turns all patches off, to
// tell a loader bug from a patch bug.
#include "patches.h"
#include <string.h>
#include <CommonCrypto/CommonDigest.h>

// ---- the list: one entry per file in patches/ ----------------------------------
extern const struct guest_patch patch_source2024_vguimatsurface, patch_css2024_launcher;
static const struct guest_patch *const all[] = {
	&patch_source2024_vguimatsurface,
	&patch_css2024_launcher,
};

int patch_jump(struct image *img, const char *fn, struct hfn *h)
{
	g32 at = macho_local_sym(img, fn);
	if (!at) return 0;
	g32 to = hfn_stub(h);
	uint8_t *p = G2H(at);
	int32_t rel = (int32_t)(to - (at + 5));
	p[0] = 0xe9;					// jmp rel32
	memcpy(p + 1, &rel, 4);
	return 1;
}

static void sha256_hex(const uint8_t *data, size_t n, char out[65])
{
	uint8_t d[CC_SHA256_DIGEST_LENGTH];
	CC_SHA256(data, (CC_LONG)n, d);
	for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

void apply_guest_patches(struct image *img)
{
	const char *path = macho_path_of(img), *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	char hash[65] = "";
	for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
		const struct guest_patch *p = all[i];
		if (strcmp(base, p->library)) continue;
		if (getenv("MAC32_NOPATCH")) {
			fprintf(stderr, "[mac32] patch for %s (%s) skipped: MAC32_NOPATCH is set\n", base, p->game);
			continue;
		}
		if (!*hash) {
			size_t n;
			const uint8_t *f = macho_file_bytes(img, &n);
			sha256_hex(f, n, hash);
		}
		if (strcmp(hash, p->sha256)) {
			LOG("patch for %s (%s) not applied: this build is %.16s..., the patch is for %.16s...\n",
			    base, p->game, hash, p->sha256);
			continue;
		}
		if (p->apply(img)) fprintf(stderr, "[mac32] patched %s (%s): %s\n", base, p->game, p->what);
		else fprintf(stderr, "[mac32] patch for %s (%s) matched the build but could not be applied\n", base, p->game);
	}
}
