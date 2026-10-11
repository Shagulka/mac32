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

## Step 1 — loader runs libtier0 (DONE 2026-10-08)
`make && ./mac32 -v -L "<P2>/bin/osx32" --tier0-test "<P2>/bin/osx32/libtier0.dylib"`:
loads Valve's i386 libtier0 at 0x13c000, runs its 11 static initializers,
`Plat_FloatTime()` measures a 0.25 s sleep as 0.255 s (double back via st(0)),
`Msg("%s %d %.2f")` prints through tier0's logging -> our vsnprintf thunk.
14 imports unresolved (C++ EH/RTTI + `_Rb_tree_*`); none called yet.

Layout of the code (src/):
- `reserve.s` + linker flags: host is non-PIE at 0x200000000, zero page 0x1000,
  and a `__RESERVE` zerofill segment (prot ---) covers [0x1000, 0xf0000000) so
  the kernel keeps the low 4 GB free from exec onward.
- `lowmem.c`: page blocks + 16-byte-aligned guest malloc inside that range.
- `tramp.s`: copied to a low page. `t_call32` (64->32, nests; saved host rsp in
  a pthread TSD slot read via %gs) and `t_hostcall32` (32->64; import stubs are
  `push idx; jmp t_hostcall32`; the host thunk runs on the guest stack).
- `invoke.s`: generic SysV call with marshalled register/stack args.
- `thunks.c` + `libc.c`, `pthread.c`, `cxx.c`, `format.c`: host functions.
  Signature strings for plain functions; raw thunks where i386 layouts differ
  (FILE, tm, timeval, timespec, sysctl size_t*, pthread objects, varargs).
- `macho.c`: classic relocs + indirect symbols + 5-byte `__jump_table` stubs,
  and LC_DYLD_INFO opcodes; binds everything eagerly; runs LC_ROUTINES and
  `__mod_init_func`; `dlopen`/`dlsym` from the guest load more images.

More Rosetta / macOS facts:
8. With `-pagezero_size 0x1000` the kernel places the host's early mallocs in
   low memory, before main(). A MAP_FIXED reservation in main() wipes them
   (crash at 0x43000). The `__RESERVE` segment fixes it. The kernel maps that
   zerofill as 128 MB vm entries, so walk them all when checking.
9. A host function far from 0x1000 cannot reference a symbol in `__RESERVE`
   (32-bit RIP-relative out of range) — check it with mach_vm_region instead.
10. `%gs`-relative TSD reads in 64-bit mode still work after a trip through
    32-bit mode under Rosetta.
11. x87 st(0) carries across the mode switch: `fldl` in 64-bit code before
    `iretq` gives the 32-bit caller its double.

## Step 2 — portal2_osx runs the real game code (2026-10-08)
`make && tools/run_p2.sh 60` (adds `-nosound -novid -windowed`). The game now:
loads ~25 of its libraries plus client/server/matchmaking; runs the Cocoa
launcher (NSApplication subclass, menus, AppWindow) through the ObjC bridge;
initializes the material system, UI (vgui2 + ATSUI-on-CoreText fonts) and
creates the game window with a working OpenGL context ("MTGL detected");
loads and initializes client.dylib and server.dylib (protobuf, convars);
then stops in SV_InitGameDLL with Portal 2's own
"Sys_Error: Steam is not running. You must start Steam in order to play this game."

Why: Mac Steam (running, PID in registry.vdf) ships steamclient.dylib as
x86_64+arm64 only; the game's 32-bit libsteam_api.dylib cannot load it.
We do NOT fake Steam or patch the check (that would bypass ownership). Next
step is a bridge: guest libsteam_api -> host 64-bit Steam client, so real
Steam checks that the user owns app 620.

What is stubbed or skipped for now (each says so at runtime):
- sound: Component Manager/AudioUnit/AudioQueue/OpenAL return "no device";
  run with -nosound. Bridging needs 32-bit render callbacks into AudioUnit.
- libcef (in-game browser): its static initializers are skipped
  (MAC32_SKIP_INIT, default "libcef.dylib") and cef_initialize & co. are
  overridden ("!name" host entries win over guest exports) to report failure.
- Bink video: Sound/Time Manager and QuickDraw return errors; use -novid.
- curl, CFNetwork proxies: fail cleanly. Gamepads / force feedback: none.
- C++ exceptions: __cxa_throw stops with the thrown type (no 32-bit unwinder).
- Carbon event/window calls: report "no events" (the Cocoa launcher pumps events).

Bridges built in this step (src/):
- objc.m: legacy i386 ObjC runtime bridge. Reads __OBJC (__module_info,
  __message_refs, __cls_refs, categories), uniques selectors, makes a real
  host subclass per guest class (methods installed as _objc_msgForward,
  forwardInvocation: calls the 32-bit IMP), forwards guest messages to host
  objects with NSInvocation using the host type signature. i386 rules:
  CGFloat=float (NSRect/NSPoint/NSSize are float structs), NSInteger=int,
  BOOL=char, NSRect returns via objc_msgSend_stret (callee pops), floats via
  objc_msgSend_fpret (st0), NSPoint/NSSize in edx:eax. stringWithFormat: is
  variadic, handled by our formatter. countByEnumeratingWithState: converted.
- apple.m: CF/CG/IOKit/Carbon/CGL; ats.m: ATS/ATSUI subset on CoreText,
  CGBitmapContext (pixels always in guest memory), Carbon pasteboard.
- gl_thunks.c generated by tools/gen_gl.py from SDK headers (971 functions
  found at runtime via dlsym); gl_special.c: pointer arrays, glGetString,
  mapped buffers via low "shadow" copies pushed back on flush/unmap.
- libc2.c / libc3.c: stat (old 96-byte and $INODE64 layouts), scanf, iconv,
  ctype tables, sockets (timeval/hostent/passwd layouts), OSAtomic, wide
  printf, scandir with guest callbacks, Classic Mac OS calls for Bink.
- guest/rt.c + rtstring.c: 32-bit runtime: RB-tree, RTTI + __dynamic_cast,
  setjmp/longjmp, GNU COW std::string/std::wstring (~97 functions).

More facts learned:
12. Never wrap forwarded messages in @autoreleasepool: returned objects belong
    to the guest's own pools.
13. Retain the NSInvocation across -invoke: the message may be -release of the
    very pool that owns the invocation (heap corruption otherwise).
14. Weak imports that do not exist bind to 0, like dyld; the engine checks only
    some of them (CGDisplayCopyDisplayMode family) - provide the whole family.
15. Nested guest calls must place arguments well below the *real* rsp, not
    below __builtin_frame_address (inlining made that 256-byte gap vanish).
16. Pointer args of type ^{CGImage=} etc. carry handles: unwrap any pointer
    that is a known handle.
17. libtogl dlopens /System/.../libGL.dylib by path: dlopen of a system path
    returns a "host libraries" handle; dlsym on it uses the host table.
18. Crash reports print the last 32 host calls of the thread and the guest
    ebp chain with symbol names; unimplemented calls print the guest stack.

## Step 3 — Steam bridge, main menu renders (2026-10-08)
`make && tools/run_p2.sh 120` now reaches Portal 2's main menu: live 3D
background map, logo, menu (docs/first-light-main-menu.jpg). Steam reports
`CClientSteamContext logged on = 1` with the user's real Steam ID.

Steam bridge (src/steam.c, generated src/steam_ifaces.c):
- The game's 32-bit libsteam_api finds Steam over Mach IPC
  (bootstrap_look_up + mach_msg pass straight through: port names and message
  headers are 32-bit everywhere), then dlopens steamclient.dylib. That dlopen
  returns STEAM_HANDLE; dlsym on it gives host thunks for CreateInterface,
  Steam_BGetCallback, Steam_FreeLastCallback, Steam_GetAPICallResult,
  Steam_ReleaseThreadLocalMemory (+ Breakpad no-ops) that call the real x86_64
  steamclient.dylib inside Steam.app. Steam does every check itself.
- Interfaces reach the guest as proxies: guest block whose vptr is a guest
  vtable of host stubs ('C' closure kind). Each stub unwraps this, takes the
  host vtable entry, converts args by signature, wraps returned interfaces by
  the pchVersion argument.
- Portal 2 was rebuilt (2024) against Steamworks SDK ~1.55: SteamClient020,
  SteamUser021, SteamFriends017, SteamUtils010, SteamInput006, UGC016,
  NetworkingSockets012... Method signatures come from steam_api.json
  (Steamworks.NET 20.1.0/20.0.0, MIT, third_party/). The JSON omits
  STEAM_PRIVATE_API vtable slots (ISteamClient::RunFrame,
  DEPRECATED_GetISteamUnifiedMessages, Set_SteamAPI_CCheckCallbackRegisteredInProcess,
  ISteamUtils::GetCSERIPPort/RunFrame, ISteamGameServer::InitGameServer ...),
  so vtable indices are read from the game's own libsteam_api flat wrappers
  (`SteamAPI_ISteamX_Method` = `mov (this),vt; jmp *off(vt)`), 931 decoded.
- Callback payloads are #pragma pack(4) on macOS: same bytes in 32/64-bit;
  only CallbackMsg_t (has a pointer) is converted.
- Not bridged (stop with a clear message if called): server-browser response
  interfaces, SteamParamStringArray_t args, struct returns (Input action data),
  guest callback function pointers (passed as NULL), ISteamGameStats/
  GameCoordinator added by hand from the public headers.
- run_p2.sh sets SteamAppId=620, as Steam does when it launches the game.

Other things this step needed: zlib (z_stream twin per guest stream), CF
collections/strings/bundles/prefs, CFRange by value, kCFType*CallBacks
mapping, old dyld NSSymbol API for Miles, MP semaphores, suspended pthreads
(pthread_create_suspended_np + thread_resume), GL buffer shadows per buffer
name (freed on glDeleteBuffers), ATSUI sentinels kATSUFromTextBeginning/
kATSUToTextEnd, ATSLayoutRecord is 2-byte packed (14 bytes), text colour is
kATSURGBAlphaColorTag (288, 4 floats). Mac32.app bundle (com.khesin.mac32)
gives the window an app identity for window tools.

## Step 4 — in game (2026-10-08 ~19:58)
Menu text fixed (colour tag 288, 14-byte ATSLayoutRecord). Keyboard input
works: Return opened Single Player and Continue Game loaded the user's
chapter 9 save; the level renders with lighting, models, viewmodel (portal
gun with PotatOS) and subtitles (docs/in-game-chapter9.jpg).
Launch: `./play_portal2.sh` (needs Steam running; log in ~/Library/Logs).
Not yet checked or not working:
- sound: not bridged (-nosound).
- mouse clicks sent to the window while it was in the background did not
  register (keyboard did); untested with the app in front. Mouse look uses
  CGGetLastMouseDelta / NSEvent deltas - untested.
- in-game web pages (CEF) off; Bink movies off (-novid); performance not measured.

## Step 5 — sound, videos, exceptions (2026-10-08 evening)
- Sound: the engine's CAudioDeviceAudioQueue runs on an AudioQueue bridge
  (src/audio.c): host output callback -> guest AudioCallback; each
  AudioQueueBuffer has a 28-byte i386 twin with guest sample memory, copied to
  the host buffer on enqueue; property listeners are trampolines. Verified by
  enqueued peak levels (menu music ~600-1200). Source pauses sound while its
  window is not the active app (Pause() -> AudioQueueStop), so background test
  runs are silent; SetFrontProcess now also asks AppKit to activate the app.
- Bink videos play (intro movie decoded and shown). They needed the Time
  Manager (TMTask 2-byte packed, tmAddr@6; PrimeTime via dispatch_after) and,
  for their sound, Sound Manager channels on AudioQueue (src/sndmgr.c:
  bufferCmd/callBackCmd/quiet/flush/pause/resume/volume; extSH/cmpSH/stdSH;
  16-bit order picked by waveform smoothness: Bink gives native order).
- C++ exceptions + RTTI: LLVM 17 libunwind + libc++abi built for i386
  (guest/cxxrt/build.sh, sparse checkout in ~/Developer/mac32-deps) as
  guest/libmac32cxx.dylib, loaded at startup. Replaces the placeholder RTTI
  and __dynamic_cast. libunwind finds unwind info via a host
  _dyld_find_unwind_sections answered by the loader (__eh_frame and
  __unwind_info of the guest image). Needed because the engine's MP3 header
  parser throws/catches in normal play. Self-test: throw/catch int, 10-frame
  unwind, rethrow, cross and down dynamic_cast all pass.
- Guest -> host call costs 53 ns (selftest benchmark); not a bottleneck.
- Mouse: the launcher's mouseDown: reads GetCurrentEventButtonState() and
  mouseMoved: uses [NSEvent mouseLocation] (global state), so synthetic clicks
  from automation tools never register; now emulated from NSApp.currentEvent
  plus pressedMouseButtons. Needs a test with a real mouse.
- Frame rate in background/locked-screen runs ~15 fps at ~10% CPU (the game is
  waiting, i.e. throttled while not visible); needs a foreground measurement.
- run_p2.sh: NOSOUND= / NOVID= / GAMEARGS= / ACTIVATE=<s> / SHOT=<s,...>;
  MAC32_TRACE=sub1,sub2 filters -t to matching host calls and selectors.

## Step 6 — confirmed by the user (2026-10-08)
Boris played it with ./play_portal2.sh: mouse works, frame rate is fine,
"everything just works". The earlier ~15 fps and dead clicks were only the
background/locked-screen test setup.

## Saves backup (before the first level load, 2026-10-08 19:55)
- `~/.universal-modder/backups/portal2-saves/20261008-195500.zip` (portal2/SAVE)
- `~/.universal-modder/backups/portal2-cfg/20261008-195500.zip` (portal2/cfg)
Restore: `um backup restore portal2-saves` / `um backup restore portal2-cfg`.

## Plan
1. (done) Loader core: parse i386 Mach-O (LC_SEGMENT, LC_DYLD_INFO rebase/bind/lazy-bind,
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
6. Steam: bridge the guest's libsteam_api to the running 64-bit Steam client
   (never a fake Steam: the ownership check must stay with Steam).

## Gotchas
- macOS has no `timeout` command; run lldb with the Bash tool timeout instead.
- `lldb --batch -o 'run ldt'` works under Rosetta and gives the fault address.

## Step 7: the game quit after a while (2026-10-09)
- Symptom: Portal 2 closed by itself after a long session, no crash report.
  Both saved logs end with `[mac32] fatal: out of low memory (12288 bytes asked)`.
- Cause: `low_pages` was a bump pointer. Blocks over 2 KB, guest `munmap`,
  aligned blocks and every thread's 8 MB guest stack were never given back, so
  the 4 GB space ran out. GCD retires idle worker threads, and each one that
  ran guest code took a fresh stack.
- Fix: freed ranges go on a sorted, merged list that `low_pages` reuses first
  fit; `low_free`/`low_block_len` track live blocks; thread stacks go back from
  a pthread key destructor; 2-32 KB sizes come from 512 KB slabs. Guest FILE
  slots (4096) are now reused too; they would have been the next limit.
- `MAC32_MEMSTATS=<seconds>` logs use. Self-test churns 40 GB of 1 MB blocks,
  a million mixed allocations and 2000 threads.
- Soak test: `-usercon +ip 127.0.0.1 +rcon_password ...` and an RCON script
  that sends `map ...` every 45 s (command-line `wait` aliases do not advance).

## Step 8: Half-Life and Half-Life 2 (2026-10-09)
- Half-Life 25th anniversary build: all i386, SDL2 2.x with ARC, blocks and
  properties. Its `libsteam_api` is a 2026 fat build that no longer exports the
  old `SteamApps()`-style accessors the engine imports, so it could not load on
  a real 32-bit Mac either; mac32 supplies them from the versioned
  `SteamAPI_SteamApps_v008()` etc.
- Valve's i386 SDL2 declares `@property`s with no accessors and no ivars
  (`SDL_VideoData`, `SDL_WindowData`); mac32 stores them as associated objects.
- `stat$INODE64` is 108 bytes on i386, not 144: writing 144 smashed the stack.
- Type strings: keep struct names (an `l`->`i` pass turned `SDL_Window` into
  `SDI_Window`), pointers to structs become `^v`, NSRange widens to `QQ`.
- Result: menu, New Game dialog and the first map (`+map c1a0`) work; it ran in
  the map 11 minutes without errors. Open: an intermittent
  unrecognized-selector exception on `SDLTranslatorResponder` (the selector
  pointer is junk). The exception preprocessor now prints the throwing stack.
- Half-Life 2 (Nov 25 2024 build): Steam and the engine start (SteamClient021
  borrows SteamClient020's names, slots probed from its own wrappers), then
  vguimatsurface crashes: `CFontManager::CreateOrFindWin32Font` stores a NULL
  font and returns NULL. `COSXFont` exists only as template leftovers, so this
  Mac build has no font code at all. Running it needs a font layer.

## Step 9: more programs (2026-10-09)
Downloads (official servers) live in `~/Developer/mac32-apps`, outside the repo:
VLC 1.1.12 and 1.0.5 (videolan.org), Firefox 3.6.28 (mozilla.org), OpenTTD
1.1.5 + OpenGFX 0.4.4 (openttd.org). All i386 (Firefox/OpenTTD also ppc).
- OpenTTD (10.4 SDK, fixed-address executable at 0x1000-0xa86000):
  - our own pages now start at 0x10000000; the gap below goes to the allocator
    after the program is mapped (low_exe_placed)
  - classic relocation r_address counts from segment 0 (__PAGEZERO), not from
    __TEXT: external fixups were landing 4 KB off and overwrote data tables
  - guest pthread_exit longjmps back to thread_main (a host pthread_exit inside
    a 32-bit call wedges Rosetta at 100% CPU)
  - getRectsBeingDrawn:count: and NSTrackingRectTag need conversion (host
    pointer arrays, 64-bit tags)
  - runs with `-s null -m null` (cwd = its folder, OpenGFX in `data/`)
- VLC 1.1.12: loads all plugins, main window up.
  - missing data imports that are host objects (NSFontAttributeName ...) are
    looked up with dlsym; before, the program read a code stub as a pointer
  - system class handles carry an i386 class layout: legacy `[super x]` in a
    category class method loads the class's isa (found via VLC's source,
    AppleRemote.m +allocWithZone:)
  - host AppKit runs on guest thread stacks: minimum 8 MB
  - crash handler on an alternate stack; crash reports describe addresses
- Portal 2 rechecked after all of this: logs on, loads a map, quits cleanly.
- VLC playback (paused at the user's request, 2026-10-09): command-line files
  never auto-play on the Mac in VLC 1.1 (the Cocoa interface blocks in
  [NSApp run] before libvlc_playlist_play); files come in as "open documents"
  Apple Events. With host classes now made at load (so nibs can instantiate
  VLCPlaylist etc.), AppKit sends applicationDidFinishLaunching: to Sparkle's
  SUUpdater, not VLCMain, and openFile: is never called: the app delegate ends
  up wrong. Next step there: trace NSApp setDelegate: during nib loading.

## Step 10: Portal (Nov 2024 Source build) - on hold at the user's request
- Same vguimatsurface.dylib as Half-Life 2. Its font manager calls a newer
  IVGuiFontImpl layout than the leftover COSXFont implements, and font
  creation was compiled out (it stores a NULL font). src/patches.c patches
  CFontManager::CreateOrFindWin32Font in memory at load; src/srcfont.m is a
  CoreText font with the layout read from the callers (offsets in its header).
- Steam: SDK 1.65 JSON added (SteamClient021, SteamUser023, Timeline V004...);
  SteamClient021 private slots measured; probing only when the game's
  libsteam_api has the accessor for that exact interface version; opaque
  void* handles (HServerListRequest) map to handles.
- termios conversion. Now Portal reaches its frame loop (console, menus load)
  and stops at shm_open (not bridged yet). HL2 should be at the same point.

## Step 11: a wine-like front end (2026-10-09)
- `bin/mac32` (bash 3.2, copied into Mac32.app/Contents/Resources): run, check,
  scan, config (per-app args/cwd/env in ~/Library/Application Support/mac32),
  wrap (launcher app in ~/Applications/mac32 with the target's icon; it runs
  the engine through a symlink inside itself so the Dock shows its name),
  logs, selftest. `make install` links it into ~/.local/bin.
- Mac32.app opened with no program: takes an Open With / dropped app, or
  shows a file panel, then execs `Resources/mac32 run <path>`.
  CFBundleDocumentTypes: application bundles and Unix executables (Alternate).
- License changed to GPL-3.0-or-later at the user's request.
- 2026-10-11: with shm_open added, Portal shows its main menu and loads
  testchmb_a_00 (portals render); Half-Life 2 shows its menu and loads
  d1_trainstation_01. Clicks sent from outside do not register (SDL reads the
  real mouse state), so maps were started with +map. The HL2 logo font
  ("HALF-LIFE²") shows a wrong last glyph: the game's custom font file is
  probably not registered with CoreText yet.
- Fixed: the game's resource/*.ttf fonts were never registered (the Mac build
  skips it); srcfont.m registers them for the process at the first font and
  matches names strictly (family, full or PostScript), so "HalfLife2" draws the
  real logo.
