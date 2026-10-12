#!/bin/bash
# Build LLVM 17 libc++ as 32-bit guest code (libmac32cxx2.dylib), for programs
# that link /usr/lib/libc++.1.dylib (Valve's 2014-and-later Mac builds).
# The C++ ABI (exceptions, RTTI) comes from libmac32cxx.dylib; operator new
# and the C library come from mac32 itself, found at load time.
set -e
L=${LLVM:-$HOME/Developer/mac32-deps/llvm-project}
[ -d "$L/libcxx/src" ] || { echo "libc++ sources not found in $L/libcxx/src: run tools/get-llvm.sh" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
mkdir -p obj/cxx
FLAGS="-target i386-apple-macos10.7 -O2 -fno-stack-protector -fPIC -funwind-tables -w -std=c++20 -nostdinc++
  -fexceptions -frtti -fvisibility-inlines-hidden
  -I$here/include -I$L/libcxx/include -I$L/libcxx/src -I$L/libcxxabi/include
  -D_LIBCPP_BUILDING_LIBRARY -DLIBCXX_BUILDING_LIBCXXABI -D_LIBCPP_DISABLE_NEW_DELETE_DEFINITIONS -DNDEBUG"
objs=()
for f in algorithm any atomic bind charconv chrono condition_variable condition_variable_destructor exception \
	 functional future hash ios ios.instantiations iostream legacy_debug_handler legacy_pointer_safety locale \
	 memory mutex mutex_destructor new_handler optional random random_shuffle regex shared_mutex stdexcept \
	 string strstream system_error thread typeinfo valarray variant vector verbose_abort; do
	o=obj/cxx/$f.o
	if clang++ $FLAGS -c "$L/libcxx/src/$f.cpp" -o "$o" 2> "obj/cxx/$f.err"; then objs+=("$o")
	else echo "libc++: $f.cpp did not build (obj/cxx/$f.err)" >&2; fi
done
printf "cxxrt/%s\n" "${objs[@]}" > obj/cxx/list
