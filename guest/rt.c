// mac32 guest runtime: 32-bit code that the old libstdc++/libgcc provided and
// that cannot be a host thunk. Written from the Itanium C++ ABI and the
// textbook red-black tree algorithms; no GNU code.
#include <stddef.h>

void mac32_guest_fatal(const char *what, const char *detail);
int strcmp(const char *, const char *);
void *malloc(unsigned long);
void free(void *);

// ---- red-black tree helpers (std::map / std::set) ----------------------------
// layout of std::_Rb_tree_node_base: color (0 red, 1 black), parent, left, right
enum { RED = 0, BLACK = 1 };
typedef struct node { int color; struct node *parent, *left, *right; } node;

node *_ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base(node *x)
{
	if (x->right) {
		x = x->right;
		while (x->left) x = x->left;
	} else {
		node *y = x->parent;
		while (x == y->right) { x = y; y = y->parent; }
		if (x->right != y) x = y;
	}
	return x;
}
const node *_ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base(const node *x)
{
	return _ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base((node *)x);
}

node *_ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base(node *x)
{
	if (x->color == RED && x->parent->parent == x) {	// header: go to rightmost
		x = x->right;
	} else if (x->left) {
		node *y = x->left;
		while (y->right) y = y->right;
		x = y;
	} else {
		node *y = x->parent;
		while (x == y->left) { x = y; y = y->parent; }
		x = y;
	}
	return x;
}
const node *_ZSt18_Rb_tree_decrementPKSt18_Rb_tree_node_base(const node *x)
{
	return _ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base((node *)x);
}

static void rot_left(node *x, node **root)
{
	node *y = x->right;
	x->right = y->left;
	if (y->left) y->left->parent = x;
	y->parent = x->parent;
	if (x == *root) *root = y;
	else if (x == x->parent->left) x->parent->left = y;
	else x->parent->right = y;
	y->left = x;
	x->parent = y;
}
static void rot_right(node *x, node **root)
{
	node *y = x->left;
	x->left = y->right;
	if (y->right) y->right->parent = x;
	y->parent = x->parent;
	if (x == *root) *root = y;
	else if (x == x->parent->right) x->parent->right = y;
	else x->parent->left = y;
	y->right = x;
	x->parent = y;
}
void _ZSt18_Rb_tree_rotate_leftPSt18_Rb_tree_node_baseRS0_(node *x, node **root) { rot_left(x, root); }
void _ZSt19_Rb_tree_rotate_rightPSt18_Rb_tree_node_baseRS0_(node *x, node **root) { rot_right(x, root); }

// header: parent = root, left = leftmost, right = rightmost
void _ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_(int left, node *x, node *p, node *h)
{
	node **root = &h->parent;
	x->parent = p;
	x->left = x->right = 0;
	x->color = RED;
	if (left) {
		p->left = x;
		if (p == h) { h->parent = x; h->right = x; }
		else if (p == h->left) h->left = x;
	} else {
		p->right = x;
		if (p == h->right) h->right = x;
	}
	while (x != *root && x->parent->color == RED) {
		node *xpp = x->parent->parent;
		if (x->parent == xpp->left) {
			node *y = xpp->right;
			if (y && y->color == RED) {
				x->parent->color = BLACK;
				y->color = BLACK;
				xpp->color = RED;
				x = xpp;
			} else {
				if (x == x->parent->right) { x = x->parent; rot_left(x, root); }
				x->parent->color = BLACK;
				xpp->color = RED;
				rot_right(xpp, root);
			}
		} else {
			node *y = xpp->left;
			if (y && y->color == RED) {
				x->parent->color = BLACK;
				y->color = BLACK;
				xpp->color = RED;
				x = xpp;
			} else {
				if (x == x->parent->left) { x = x->parent; rot_right(x, root); }
				x->parent->color = BLACK;
				xpp->color = RED;
				rot_left(xpp, root);
			}
		}
	}
	(*root)->color = BLACK;
}

// Unlink z from the tree rooted at header h and rebalance; returns the node
// the caller must free (z itself).
node *_ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_(node *z, node *h)
{
	node **root = &h->parent, **leftmost = &h->left, **rightmost = &h->right;
	node *y = z, *x, *xp;
	if (!y->left) x = y->right;
	else if (!y->right) x = y->left;
	else {
		y = y->right;
		while (y->left) y = y->left;
		x = y->right;
	}
	if (y != z) {
		// y is z's successor: move y into z's place
		z->left->parent = y;
		y->left = z->left;
		if (y != z->right) {
			xp = y->parent;
			if (x) x->parent = y->parent;
			y->parent->left = x;
			y->right = z->right;
			z->right->parent = y;
		} else {
			xp = y;
		}
		if (*root == z) *root = y;
		else if (z->parent->left == z) z->parent->left = y;
		else z->parent->right = y;
		y->parent = z->parent;
		int c = y->color; y->color = z->color; z->color = c;
		y = z;
	} else {
		xp = y->parent;
		if (x) x->parent = y->parent;
		if (*root == z) *root = x;
		else if (z->parent->left == z) z->parent->left = x;
		else z->parent->right = x;
		if (*leftmost == z) {
			if (!z->right) *leftmost = z->parent;
			else { node *m = x; while (m->left) m = m->left; *leftmost = m; }
		}
		if (*rightmost == z) {
			if (!z->left) *rightmost = z->parent;
			else { node *m = x; while (m->right) m = m->right; *rightmost = m; }
		}
	}
	if (y->color != RED) {
		while (x != *root && (!x || x->color == BLACK)) {
			if (x == xp->left) {
				node *w = xp->right;
				if (w->color == RED) {
					w->color = BLACK;
					xp->color = RED;
					rot_left(xp, root);
					w = xp->right;
				}
				if ((!w->left || w->left->color == BLACK) && (!w->right || w->right->color == BLACK)) {
					w->color = RED;
					x = xp;
					xp = xp->parent;
				} else {
					if (!w->right || w->right->color == BLACK) {
						w->left->color = BLACK;
						w->color = RED;
						rot_right(w, root);
						w = xp->right;
					}
					w->color = xp->color;
					xp->color = BLACK;
					if (w->right) w->right->color = BLACK;
					rot_left(xp, root);
					break;
				}
			} else {
				node *w = xp->left;
				if (w->color == RED) {
					w->color = BLACK;
					xp->color = RED;
					rot_right(xp, root);
					w = xp->left;
				}
				if ((!w->right || w->right->color == BLACK) && (!w->left || w->left->color == BLACK)) {
					w->color = RED;
					x = xp;
					xp = xp->parent;
				} else {
					if (!w->left || w->left->color == BLACK) {
						w->right->color = BLACK;
						w->color = RED;
						rot_left(w, root);
						w = xp->left;
					}
					w->color = xp->color;
					xp->color = BLACK;
					if (w->left) w->left->color = BLACK;
					rot_right(xp, root);
					break;
				}
			}
		}
		if (x) x->color = BLACK;
	}
	return y;
}

unsigned _ZSt20_Rb_tree_black_countPKSt18_Rb_tree_node_baseS1_(const node *n, const node *root)
{
	unsigned c = 0;
	if (!n) return 0;
	do {
		if (n->color == BLACK) c++;
		if (n == root) break;
		n = n->parent;
	} while (1);
	return c;
}

const char _ZSt7nothrow = 0;	// std::nothrow (an empty struct)

// ---- RTTI ----------------------------------------------------------------------
// type_info objects point at vtable+8. Their vtables only need to be distinct
// so __dynamic_cast can tell the three class kinds apart.
typedef struct tinfo { const void *vptr; const char *name; } tinfo;
typedef struct si_tinfo { tinfo t; const tinfo *base; } si_tinfo;
typedef struct base_info { const tinfo *type; long offset_flags; } base_info;
typedef struct vmi_tinfo { tinfo t; unsigned flags, nbases; base_info bases[1]; } vmi_tinfo;

static void tinfo_dtor(void *p) { (void)p; }
const void *_ZTVN10__cxxabiv117__class_type_infoE[6] = { 0, 0, (void *)tinfo_dtor, (void *)tinfo_dtor, 0, 0 };
const void *_ZTVN10__cxxabiv120__si_class_type_infoE[6] = { 0, 0, (void *)tinfo_dtor, (void *)tinfo_dtor, 0, 0 };
const void *_ZTVN10__cxxabiv121__vmi_class_type_infoE[6] = { 0, 0, (void *)tinfo_dtor, (void *)tinfo_dtor, 0, 0 };
const void *_ZTVN10__cxxabiv123__fundamental_type_infoE[6] = { 0, 0, (void *)tinfo_dtor, (void *)tinfo_dtor, 0, 0 };
const void *_ZTVN10__cxxabiv119__pointer_type_infoE[6] = { 0, 0, (void *)tinfo_dtor, (void *)tinfo_dtor, 0, 0 };
const void *_ZTVSt9type_info[6] = { 0, 0, (void *)tinfo_dtor, (void *)tinfo_dtor, 0, 0 };

#define FUND(sym, nm) const tinfo sym = { &_ZTVN10__cxxabiv123__fundamental_type_infoE[2], nm }
FUND(_ZTIi, "i"); FUND(_ZTIj, "j"); FUND(_ZTIl, "l"); FUND(_ZTIm, "m"); FUND(_ZTIc, "c");
FUND(_ZTIh, "h"); FUND(_ZTIb, "b"); FUND(_ZTIf, "f"); FUND(_ZTId, "d"); FUND(_ZTIx, "x");
FUND(_ZTIy, "y"); FUND(_ZTIv, "v"); FUND(_ZTIs, "s"); FUND(_ZTIt, "t");
static const tinfo char_tinfo = { &_ZTVN10__cxxabiv123__fundamental_type_infoE[2], "c" };
const struct { tinfo t; unsigned flags; const tinfo *pointee; } _ZTIPKc =
	{ { &_ZTVN10__cxxabiv119__pointer_type_infoE[2], "PKc" }, 1, &char_tinfo };

#define KIND(t) ((t)->vptr)
static int same_type(const tinfo *a, const tinfo *b)
{
	return a == b || (a->name == b->name) || !strcmp(a->name, b->name);
}

// Collect every public subobject of type dst inside the object at obj (of type t).
static void find_all(char *obj, const tinfo *t, const tinfo *dst, char **hits, int *n)
{
	if (same_type(t, dst)) {
		for (int i = 0; i < *n; i++) if (hits[i] == obj) return;	// shared virtual base
		if (*n < 32) hits[(*n)++] = obj;
		return;
	}
	if (KIND(t) == &_ZTVN10__cxxabiv120__si_class_type_infoE[2]) {
		find_all(obj, ((const si_tinfo *)t)->base, dst, hits, n);
	} else if (KIND(t) == &_ZTVN10__cxxabiv121__vmi_class_type_infoE[2]) {
		const vmi_tinfo *v = (const vmi_tinfo *)t;
		for (unsigned i = 0; i < v->nbases; i++) {
			long of = v->bases[i].offset_flags, off = of >> 8;
			if (!(of & 2)) continue;			// not public
			if (of & 1) off = *(long *)(*(char **)obj + off);	// virtual: offset from vtable
			find_all(obj + off, v->bases[i].type, dst, hits, n);
		}
	}
}

void *__dynamic_cast(const void *src, const tinfo *src_type, const tinfo *dst_type, long src2dst)
{
	(void)src_type;
	if (!src) return 0;
	void **vt = *(void ***)src;
	const tinfo *whole_type = (const tinfo *)vt[-1];
	char *whole = (char *)src + (long)vt[-2];
	if (same_type(whole_type, dst_type)) return whole;
	char *hits[32];
	int n = 0;
	find_all(whole, whole_type, dst_type, hits, &n);
	if (src2dst >= 0)
		for (int i = 0; i < n; i++) if (hits[i] == (char *)src - src2dst) return hits[i];
	return n == 1 ? hits[0] : 0;
}

// ---- exceptions: not supported yet; say what was thrown and stop ------------
void *__cxa_allocate_exception(unsigned long n) { return (char *)malloc(n + 64) + 64; }
void __cxa_free_exception(void *p) { free((char *)p - 64); }
void __cxa_throw(void *obj, const tinfo *t, void (*dtor)(void *))
{
	(void)obj; (void)dtor;
	mac32_guest_fatal("C++ exception thrown", t ? t->name : "?");
}
void *__cxa_begin_catch(void *e) { (void)e; mac32_guest_fatal("__cxa_begin_catch", ""); return 0; }
void __cxa_end_catch(void) { mac32_guest_fatal("__cxa_end_catch", ""); }
void __cxa_rethrow(void) { mac32_guest_fatal("__cxa_rethrow", ""); }
void __cxa_call_unexpected(void *e) { (void)e; mac32_guest_fatal("__cxa_call_unexpected", ""); }
int __gxx_personality_v0(void) { mac32_guest_fatal("__gxx_personality_v0 (unwinding)", ""); return 0; }
void _Unwind_Resume(void *e) { (void)e; mac32_guest_fatal("_Unwind_Resume", ""); }
void _ZSt17__throw_bad_allocv(void) { mac32_guest_fatal("std::bad_alloc", ""); }
void _ZSt20__throw_length_errorPKc(const char *s) { mac32_guest_fatal("std::length_error", s); }
void _ZSt20__throw_out_of_rangePKc(const char *s) { mac32_guest_fatal("std::out_of_range", s); }
void _ZSt24__throw_out_of_range_fmtPKcz(const char *s, ...) { mac32_guest_fatal("std::out_of_range", s); }
void _ZSt19__throw_logic_errorPKc(const char *s) { mac32_guest_fatal("std::logic_error", s); }
void _ZSt21__throw_runtime_errorPKc(const char *s) { mac32_guest_fatal("std::runtime_error", s); }
void _ZSt16__throw_bad_castv(void) { mac32_guest_fatal("std::bad_cast", ""); }
void __cxa_bad_typeid(void) { mac32_guest_fatal("std::bad_typeid", ""); }
void __cxa_bad_cast(void) { mac32_guest_fatal("std::bad_cast", ""); }

// iostream static setup: no streams are wired up yet
void _ZNSt8ios_base4InitC1Ev(void *self) { (void)self; }
void _ZNSt8ios_base4InitD1Ev(void *self) { (void)self; }
