#!/bin/bash
# Build LLVM 17 libunwind + libc++abi as 32-bit guest code (libmac32cxx.dylib).
set -e
L=${LLVM:-$HOME/Developer/mac32-deps/llvm-project}
[ -d "$L/libcxxabi" ] || { echo "LLVM sources not found in $L: run tools/get-llvm.sh (or set LLVM=)" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
mkdir -p obj
COMMON="-target i386-apple-macos10.7 -O2 -fno-stack-protector -fPIC -funwind-tables -w"
INC="-I$here/include -I$L/libcxx/include -I$L/libcxxabi/include -I$L/libunwind/include -I$L/libcxx/src -I$L/libunwind/src"
DEFS="-D_LIBUNWIND_IS_NATIVE_ONLY -D_LIBCXXABI_BUILDING_LIBRARY -D_LIBCPP_BUILDING_LIBRARY -DLIBCXXABI_USE_LLVM_UNWINDER -DNDEBUG"
objs=()
for f in libunwind.cpp UnwindLevel1.c UnwindLevel1-gcc-ext.c UnwindRegistersSave.S UnwindRegistersRestore.S; do
	o=obj/uw_${f%.*}.o
	case $f in
	*.cpp) clang++ $COMMON $INC $DEFS -std=c++17 -nostdinc++ -fno-exceptions -fno-rtti -c $L/libunwind/src/$f -o $o ;;
	*) clang $COMMON $INC $DEFS -std=c99 -c $L/libunwind/src/$f -o $o ;;
	esac
	objs+=($o)
done
for f in cxa_aux_runtime cxa_default_handlers cxa_exception cxa_exception_storage cxa_handlers cxa_personality \
	 cxa_vector private_typeinfo stdlib_exception stdlib_typeinfo abort_message fallback_malloc; do
	o=obj/abi_$f.o
	clang++ $COMMON $INC $DEFS -std=c++20 -nostdinc++ -fexceptions -frtti -c $L/libcxxabi/src/$f.cpp -o $o
	objs+=($o)
done
echo "${objs[@]}" > obj/list
