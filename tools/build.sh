#!/bin/sh
# make_arrange_rom.exe (ibara-arrange-mame): one static Windows .exe, from make_arrange_rom.c + arrpatch.h + zlib 1.2.11.
# From Git Bash on the PC:
#   MSYS_NO_PATHCONV=1 docker run --rm --memory=1g -v "<this repository>:/p" -v "<zlib 1.2.11 source folder>:/z" \
#     ps2x-mingw:1 sh /p/tools/build.sh
# ps2x-mingw:1 = the Dockerfile of ibara360's tools/ps2extract
# (github.com/PaulAtreides67/ibara360): docker build -t ps2x-mingw:1 <ibara360>/tools/ps2extract
# arrpatch.h is the patch (made from the ARRANGE ROM build by mkpatch.py, a build-time tool not in this repository). The .exe lands in tools/bin.
set -e
Z=${1:-${Z:-/z}}
cd /p/tools
mkdir -p bin
x86_64-w64-mingw32-gcc -O2 -s -static -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
	-D_FILE_OFFSET_BITS=64 -D__USE_MINGW_ANSI_STDIO=1 -I. -I"$Z" -o bin/make_arrange_rom.exe make_arrange_rom.c \
	"$Z/adler32.c" "$Z/crc32.c" "$Z/deflate.c" "$Z/trees.c" "$Z/zutil.c" "$Z/inflate.c" "$Z/inftrees.c" "$Z/inffast.c" \
	-lcomdlg32
ls -l bin/make_arrange_rom.exe
