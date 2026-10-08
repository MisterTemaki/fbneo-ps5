#!/bin/sh
# FBNeo PS5: FBNeo's generated headers (what its makefiles make before compiling), into $2.
#   gen_headers.sh <fbneo root> <out dir> <host C compiler> <host C++ compiler>
# driverlist.h lists the arcade drivers only (fbneo_sources.py); m68kops.c/.h are Musashi's generated 68000 core.
#
# SPDX-License-Identifier: MIT
set -e
ROOT=$(cd "$1" && pwd)
OUT=$2
HCC=${3:-cc}
HCXX=${4:-c++}
TOOLS=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT/tools"
OUT=$(cd "$OUT" && pwd)
S=$ROOT/src

python3 "$TOOLS/fbneo_sources.py" "$ROOT" drivers | sed "s|^|$ROOT/|" > "$OUT/tools/drivers.txt"
perl "$S/dep/scripts/gamelist.pl" -o "$OUT/driverlist.h" -l "$OUT/tools/gamelist.txt" -f "$OUT/tools/drivers.txt" > "$OUT/tools/gamelist.log"

$HCXX -O1 -o "$OUT/tools/ctv_make" "$S/burn/drv/capcom/ctv_make.cpp"
"$OUT/tools/ctv_make" > "$OUT/ctv.h"
$HCXX -O1 -o "$OUT/tools/pgm_sprite_create" "$S/burn/drv/pgm/pgm_sprite_create.cpp"
"$OUT/tools/pgm_sprite_create" > "$OUT/pgm_sprite.h"
for f in toa_gp9001_func neo_sprite_func cave_tile_func cave_sprite_func psikyo_tile_func; do
	perl "$S/dep/scripts/$f.pl" -o "$OUT/$f.h"
done

$HCC -O1 -w -o "$OUT/tools/m68kmake" "$S/cpu/m68k/m68kmake.c"
"$OUT/tools/m68kmake" "$OUT/" "$S/cpu/m68k/m68k_in.c" > "$OUT/tools/m68kmake.log"
echo "generated: $(ls "$OUT" | tr '\n' ' ')"
