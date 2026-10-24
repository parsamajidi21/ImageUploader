#!/bin/sh
#
# tests/run.sh - tests for the image reader / backup writer (src/stream.c)
#
# Every supported format must decode to exactly the original image, truncated
# compressed images must fail, and backups must equal the source.
# Run "make" in src first, the decompressor libraries are needed.
#
# Needs: cc, gzip, bzip2, xz, zstd, zip, tar, cpio. Works on Linux, macOS and MSYS2.

cd "$(dirname "$0")" || exit 1
TESTS=$(pwd)
SRC=$TESTS/../src
WORK=${TMPDIR:-/tmp}/imageuploader-tests.$$
mkdir -p "$WORK" || exit 1
trap 'rm -rf "$WORK"' EXIT

CFLAGS="-D_FILE_OFFSET_BITS=64 -std=c99 -O2 -Wall -Wextra -pedantic"
case "$(uname -s)" in
    MINGW*|MSYS*) CFLAGS="$CFLAGS -DWINVER=0x0500 -DUNICODE=1"; EXE=.exe ;;
    Darwin) CFLAGS="$CFLAGS -DMACOSX=1" ;;
esac
H=$WORK/harness$EXE
${CC:-cc} $CFLAGS -I"$SRC" -I"$SRC/zlib" -I"$SRC/bzip2" -I"$SRC/xz" -I"$SRC/zstd" -o "$H" harness.c \
    "$SRC/stream.c" "$SRC/lang.c" "$SRC/zlib/libz.a" "$SRC/bzip2/libbz2.a" "$SRC/xz/libxz.a" "$SRC/zstd/libzstd.a" || exit 1

size() { wc -c < "$1" | tr -d ' '; }
if command -v sha256sum >/dev/null; then hash() { sha256sum | cut -d' ' -f1; }; else hash() { shasum -a 256 | cut -d' ' -f1; }; fi
if tar --version 2>/dev/null | grep -q GNU; then GNUTAR=gnu; else GNUTAR=gnutar; fi

# a random image with an odd size (partial last sector) and a zero tail
cd "$WORK" || exit 1
head -c 5255225 /dev/urandom > disk.img
head -c 3000000 /dev/zero >> disk.img
N=$(size disk.img)
WANT=$(hash < disk.img)
cp disk.img disk.iso
gzip -9 -c disk.img > disk.img.gz
bzip2 -c disk.img > disk.img.bz2
xz -c disk.img > disk.img.xz
zstd -q -c disk.img > disk.img.zst
cat disk.img.xz disk.img.xz > twice.img.xz
zip -q disk.zip disk.img
zip -q -fz disk64.zip disk.img
tar --format=ustar -cf disk.tar disk.img
tar --format=pax -cf disk.pax disk.img
tar --format=pax -cf - disk.img | xz > disk.pax.xz
echo disk.img | cpio -o -H newc 2>/dev/null > disk.cpio
LONG=$(printf '%0150d' 0).img
cp disk.img "$LONG"
tar --format=$GNUTAR -cf longgnu.tar "$LONG"
tar --format=pax -cf longpax.pax "$LONG"
rm "$LONG"
for f in disk.img.gz disk.img.bz2 disk.img.xz disk.img.zst disk.zip disk.pax.xz; do
    head -c $(($(size $f) * 6 / 10)) $f > trunc-$f
done

pass=0; fail=0
ok() { pass=$((pass + 1)); echo "PASS $1"; }
ko() { fail=$((fail + 1)); echo "FAIL $1"; }

for f in disk.img disk.iso disk.img.gz disk.img.bz2 disk.img.xz disk.img.zst disk.zip disk64.zip \
         disk.tar disk.pax disk.pax.xz disk.cpio longgnu.tar longpax.pax; do
    rm -f out
    if "$H" dec $f out >/dev/null && [ "$(head -c $N out | hash)" = "$WANT" ]; then ok "decode $f"; else ko "decode $f"; fi
done
rm -f out
if "$H" dec twice.img.xz out >/dev/null && [ "$(head -c $((2 * N)) out | hash)" = "$(cat disk.img disk.img | hash)" ]; then
    ok "decode concatenated xz streams"; else ko "decode concatenated xz streams"; fi
for f in trunc-*; do
    if "$H" dec $f out >/dev/null; then ko "truncated $f must fail"; else ok "truncated $f fails"; fi
done
rm -f bak
if "$H" enc disk.img bak 0 >/dev/null && [ "$(size bak)" = "$N" ] && [ "$(hash < bak)" = "$WANT" ]; then
    ok "raw backup"; else ko "raw backup"; fi
rm -f bak
if "$H" enc disk.img bak 1 >/dev/null && [ "$(zstd -q -d -c bak | hash)" = "$WANT" ]; then
    ok "zstd backup"; else ko "zstd backup"; fi

echo "$pass passed, $fail failed"
[ $fail = 0 ]
