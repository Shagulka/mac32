# third_party

- `steam_api_1.55.json`, `steam_api_1.53.json`: the Steamworks SDK interface
  description (`steam_api.json`) as shipped in Steamworks.NET 20.1.0 and 20.0.0
  (MIT, https://github.com/rlabrecque/Steamworks.NET, CodeGen/steam/). Used only
  to generate the vtable bridge in `src/steam_ifaces.c` (`tools/gen_steam.py`).

  Their MIT license and copyright notice are in `LICENSE-Steamworks.NET.txt`.

- `guest/cxxrt/build.sh` builds LLVM's libunwind and libc++abi (llvmorg-17.0.6,
  Apache-2.0 WITH LLVM-exception) for i386 from a checkout you provide; no LLVM
  source is kept in this repository.
