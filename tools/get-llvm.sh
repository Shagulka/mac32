#!/bin/bash
# Fetch the parts of LLVM 17.0.6 that the 32-bit C++ runtime is built from
# (libunwind, libc++abi and libc++): about 45 MB, into
# ~/Developer/mac32-deps/llvm-project, or $LLVM if set.
set -e
dest=${LLVM:-$HOME/Developer/mac32-deps/llvm-project}
if [ -d "$dest/libcxx/src" ]; then echo "LLVM is already in $dest"; exit 0; fi
if [ -d "$dest/.git" ]; then git -C "$dest" sparse-checkout add libcxx/src; exit 0; fi
git clone --depth 1 --branch llvmorg-17.0.6 --filter=blob:none --sparse \
	https://github.com/llvm/llvm-project.git "$dest"
git -C "$dest" sparse-checkout set cmake runtimes libcxx/include libcxx/src libcxxabi libunwind
echo "LLVM sources are in $dest"
