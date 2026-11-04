#!/usr/bin/env bash
# Build the host simulator (see host_sim.cpp). Needs gcc/g++ (C++23), libcurl and mbedtls dev packages.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-$here/build}"
mkdir -p "$out"
mapfile -t picoc < <(find "$root/3rd-party/picoc/source/interpreter" -maxdepth 1 -name '*.c' ! -name 'debug.c' | sort)
INC=(-I"$here/include" -I"$root/3rd-party/picoc/include" -I"$root/common/script" -I"$root/common/script/cstdlib"
     -I"$root/common" -I"$root/3rd-party/json")
objs=()
for f in "${picoc[@]}" "$root/common/script/platform_checkpoint.c" "$root"/common/script/cstdlib/*.c \
         "$root/common/script/library_checkpoint.c" "$root/common/script/scriptabort.c"; do
    o="$out/c_$(basename "$f" .c).o"
    gcc -c -w -g -O0 -funwind-tables "${INC[@]}" -DUNIX_HOST -D_GNU_SOURCE=1 "$f" -o "$o"
    objs+=("$o")
done
for f in checkpoint_api json_api web_api httpcall zip_api seal_api progress_api scriptargs scriptheap scriptconsole scriptbar scriptengine; do
    g++ -c -g -O0 -std=c++23 -w "${INC[@]}" -DUNIX_HOST -include "$here/include/sim_compat.h" "$root/common/script/$f.cpp" -o "$out/x_$f.o"
    objs+=("$out/x_$f.o")
done
for f in transferprotocol common; do
    g++ -c -g -O0 -std=c++23 -w "${INC[@]}" "$root/common/$f.cpp" -o "$out/x_$f.o"
    objs+=("$out/x_$f.o")
done
g++ -c -g -O0 -std=c++23 -w "${INC[@]}" "$here/host_sim.cpp" -o "$out/host_sim.o"
g++ -o "$out/host_sim" "$out/host_sim.o" "${objs[@]}" -lcurl -lmbedcrypto -lm
echo "built $out/host_sim"
