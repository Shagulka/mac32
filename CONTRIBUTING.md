# Contributing to mac32

Thanks for helping. The most useful things are reports of how real programs
behave, and bridges for the macOS functions they call.

## Report a program

Open an issue with:

- the program's name, version, and where you got it (please only official
  downloads, and only programs and games you own);
- `mac32 check --all <app>`;
- the end of its log (`mac32 logs <app>`, or run it with `mac32 run -v`).

When a program calls a function mac32 does not have, it stops with
`unimplemented function _Name` and a stack trace into the program. That name is
the place to start.

## Add a missing function

Each function the program imports from macOS needs an entry in a table in
`src/`. There are three ways to write one.

**1. Generate it.** Much of CoreGraphics, CoreFoundation and Carbon comes from
the SDK headers:

```bash
tools/wanted.sh path/to/Some.app     # what the program needs and mac32 lacks
python3 tools/gen_api.py carbon      # also: cg, cf
python3 tools/gen_api.py carbon --why    # why a function was skipped
```

The generator writes `src/cg_thunks.c`, `src/cf_thunks.c` and
`src/carbon_thunks.c`. Do not edit those by hand. A function written by hand
anywhere else in `src/` takes its place on the next run.

**2. A generic entry.** When the arguments only need simple conversion, list the
real function with a signature string, `"<return>:<arguments>"`:

```c
G("_CFRunLoopStop", CFRunLoopStop, "v:o"),
G("_AudioUnitSetParameter", w_AudioUnitSetParameter, "i:ouuufu"),
```

| Code | Argument | Return |
|---|---|---|
| `v` | | nothing |
| `i` / `u` | 32-bit signed / unsigned integer | same |
| `l` | 64-bit integer (two stack slots) | 64-bit integer |
| `p` | pointer into the program's memory, used as is | pointer |
| `o` | object or opaque handle (CFTypeRef, `id`, ...) | handle |
| `B` / `b` | Boolean in a 4-byte slot | Boolean (low byte only) |
| `d` / `f` | double / float | same, in st(0) |
| `g` | CGFloat (float on i386, double on the host) | same |
| `P` `R` `A` | CGPoint or CGSize, CGRect, CGAffineTransform | same |
| `N` | CFRange | CFRange |
| `r` `O` `W` | CGRect pointer, object out-pointer, pointer to a `long` count | |
| `S` | | C string, copied into the program's memory |

Pass a pointer as `p` only when the data it points to has the same layout in
i386 and x86_64. A `long`, `size_t` or pointer inside a struct changes size, so
such a struct needs a hand-written entry.

**3. A raw entry.** For anything else, write the conversion yourself:

```c
static void t_Example(uint32_t *args, struct hres *r)
{
	// A(n) is the nth 32-bit argument; G2H turns a program address into a host one
	RET(Example(A(0), G2H(A(1))));
}
...
R("_Example", t_Example),
```

Callbacks into the program go through `gcall(fn, nargs, ...)`; see
`src/audio.c` for a full example with a render callback.

If an API simply cannot work on modern macOS, fail the way the API itself
defines failure (an error code, `NULL`), with a comment saying why. Programs
usually cope with that better than with a crash.

## Game patches

A bug in a game's own code, one that would stop it on any Mac, gets a patch in
`patches/`: one file per game library, matched by the SHA-256 of the exact
build. Add a row to the patch table in the README saying what is wrong and what
the patch does.

## Before you send a change

- `make` builds without warnings, and `mac32 selftest` passes.
- Programs that worked still work (Portal 2 is the long test, if you own it).
- Write comments that say why, not what. The code uses tabs and C99/Objective-C.
- Never commit game files, Apple files, or code from leaked sources.

By contributing you agree that your work is licensed under GPL-3.0-or-later,
like the rest of mac32.
