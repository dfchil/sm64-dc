#!/bin/sh
# Builds tasref (tasref.c) and its two plugins into build/tasref, against Homebrew's Mupen64Plus.
set -e
cd "$(dirname "$0")/../.."
MUPEN=${MUPEN:-/opt/homebrew}
OUT=build/tasref
mkdir -p $OUT
CFLAGS="-O2 -Wall -Wextra -I$MUPEN/include"
cc $CFLAGS -dynamiclib -o $OUT/tasref_video.dylib tools/tasref/tasref_video.c
cc $CFLAGS -dynamiclib -o $OUT/tasref_input.dylib tools/tasref/tasref_input.c
cc $CFLAGS -DMUPEN_LIB_DIR="\"$MUPEN/lib\"" -DMUPEN_DATA_DIR="\"$MUPEN/share/mupen64plus\"" \
    -o $OUT/tasref tools/tasref/tasref.c -L$MUPEN/lib -lmupen64plus
