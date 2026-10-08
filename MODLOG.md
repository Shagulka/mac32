# mac32 — run 32-bit Intel Mac apps on Apple Silicon

Goal: a loader that runs 32-bit i386 Mach-O programs (first target: Portal 2's
`portal2_osx`) on macOS 26 / Apple Silicon, in a 64-bit x86_64 process under
Rosetta 2. The 32-bit code runs as is; calls into macOS go through thunks to
the 64-bit system libraries. Same idea as CrossOver's wine32on64, but for Mach-O.

Ground rules: only the user's own games, nothing from Valve's code or files is
committed here, no leaked source code.

## Machine
- M2 Pro, 16 GB, macOS 26.3.1 (25D771280a), Xcode clang 17, SDK 26.5.
- Rosetta runtime: `/Library/Apple/usr/libexec/oah/libRosettaRuntime`.

## Target: Portal 2 (Steam app 620, build 25240193)
- Install: `~/Library/Application Support/Steam/steamapps/common/Portal 2`.
- `portal2_osx`: i386 only. Links libSystem, libstdc++.6, libgcc_s.1.
- `bin/osx32/`: 46 i386 dylibs (engine, materialsystem, libtogl, SDL2, Miles,
  Bink, libsteam_api, libcef, ...). Launched by `portal2.sh` with
  `DYLD_LIBRARY_PATH=bin/osx32`.
- Imports 1,934 distinct undefined symbols from ~27 system libs: libSystem,
  libstdc++/libgcc_s (removed from macOS), libiconv, Carbon, CoreServices,
  ApplicationServices, CoreFoundation, IOKit, AppKit, libobjc, OpenGL,
  Foundation, AudioToolbox, CoreAudio, AudioUnit, libc++, SystemConfiguration,
  QuickTime (gone), Quartz, ForceFeedback, libz, libcurl, Security, OpenAL.
- `libsteam_api.dylib` is 32-bit; Mac Steam's steamclient is 64-bit only, so
  Steam needs a bridge (or a stub for offline play).

## Step 0 — probe (DONE 2026-10-08): Rosetta runs 32-bit code
`probe/probe.c` + `probe/tramp.s`. Build:
`clang -arch x86_64 -O1 -Wl,-pagezero_size,0x1000 probe.c tramp.s -o probe`;
run `./probe ldt`. Result: a hand-written i386 function and a clang-built i386
CRC32 (`clang -target i386-apple-macos10.6 -Os -mno-sse -c`) both return the right
answers. The CRC contains `48` = `dec eax`, which would decode as a REX prefix in
64-bit mode, so the code truly ran in 32-bit mode.

What Rosetta accepts (found by trial and by `strings libRosettaRuntime`):
1. `-pagezero_size 0x1000` frees the low 4 GB; `mmap(0x10000000, MAP_FIXED)` works.
2. GDT selector 0x1b (the Intel-Mac 32-bit USER_CS) is rejected:
   `rosetta error: invalid gdt selector index 3`.
3. `i386_set_ldt(LDT_AUTO_ALLOC, ...)` is rejected ("kLdtAutoAlloc not supported"),
   and so is more than one selector per call. An explicit index works:
   `i386_set_ldt(16, &desc, 1)` returns 16, selector 0x87. The descriptor is a
   plain flat 32-bit code segment (type 0x1a, dpl 3, D=1, G=1, limit 0xfffff).
4. Entering 32-bit mode with `ljmpl *mem` from 64-bit code does NOT switch mode:
   Rosetta kept running in 64-bit mode (the 32-bit `ret` popped 8 bytes and
   jumped to 0x5_10000052). `iretq` with a frame {rip, cs=0x87, rflags, rsp, ss}
   DOES switch — that is what Wine uses too (signal_x86_64.c).
5. Leaving 32-bit mode with `push $0x2b; push eip; lret` works.
6. Loading ds/es with 0x23 in 64-bit mode is accepted.
7. RWX pages made with mprotect are fine; the trampoline writes its own data.

## Plan
1. Loader core: parse i386 Mach-O (LC_SEGMENT, LC_DYLD_INFO rebase/bind/lazy-bind,
   LC_SYMTAB/DYSYMTAB for older indirect symbols), map into low 4 GB, apply
   rebases, run `__mod_init_func`, find exports. Test on a small Portal 2 dylib
   (`libvstdlib.dylib`/`libtier0.dylib`) — see which imports it needs.
2. 32→64 call path: each imported symbol binds to a 32-bit stub that far-returns
   into 64-bit code with a thunk ID; the thunk reads cdecl args from the 32-bit
   stack, calls the real 64-bit function, converts results. 64→32 path for
   callbacks (qsort, pthread_create, atexit, SDL/GL callbacks).
3. Low-memory heap: malloc/new for the 32-bit side must return < 4 GB pointers.
4. Thunk set, in order of need: libSystem (libc, pthread, file, mmap, time),
   libstdc++ (old GNU ABI: reimplement or build a 32-bit libstdc++ from GCC
   source and load it as 32-bit code), then SDL2-facing libs, OpenGL, CoreAudio,
   Cocoa/objc (hardest: 32-bit objc runtime structures).
5. TLS: 32-bit code on Darwin reads thread data via %gs; Rosetta refuses a
   nonzero GS ("setting GS to a nonzero selector is unsupported") — check how
   i386 libSystem code reaches TLS (pthread_getspecific via thunk is fine; inline
   `%gs:` reads in game code need patching or emulation).
6. Steam: stub libsteam_api for offline, later bridge to 64-bit steamclient.

## Gotchas
- macOS has no `timeout` command; run lldb with the Bash tool timeout instead.
- `lldb --batch -o 'run ldt'` works under Rosetta and gives the fault address.
