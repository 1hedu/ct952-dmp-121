#!/bin/bash
# JupiterSDK on CT952 — verification driver.
#
# 1. Builds and runs the renderer + audio tests natively (little-endian).
# 2. Builds them as static 64-bit SPARC binaries and runs them under
#    qemu-sparc64 (big-endian). CRCs must match the native run exactly —
#    this is the endianness proof for all ported portable code.
# 3. Compile-checks every firmware-facing Jupiter source with a 32-bit
#    big-endian SPARC V8 GCC (-m32 -mcpu=v8, the CT952's ABI) against
#    the real firmware headers, using the eCos header stubs in
#    ecos_stub/ (only needed because the eCos install isn't present).
#
# Requirements: gcc, sparc64-linux-gnu-gcc, qemu-sparc64 (qemu-user).
set -u
cd "$(dirname "$0")"

JUP=..
REPO=../..
FAIL=0

echo "=== 1. native (little-endian) tests ==="
mkdir -p build/native
gcc -O2 -Wall -DJUP_HOST_BUILD -I"$JUP" \
    test_render.c "$JUP/jnes.c" "$JUP/jgb.c" "$JUP/jrgb2yuv.c" \
    -o build/native/test_render || FAIL=1
gcc -O2 -Wall -DJUP_HOST_BUILD -I"$JUP" \
    test_render2.c "$JUP/jsnes.c" "$JUP/jgen.c" \
    -o build/native/test_render2 || FAIL=1
gcc -O2 -Wall -DJUP_HOST_BUILD -I"$JUP" \
    test_fb.c "$JUP/jfb.c" "$JUP/jrgb2yuv.c" \
    -o build/native/test_fb || FAIL=1
gcc -O2 -Wall -DJUP_HOST_BUILD -I"$JUP" \
    test_gpu.c "$JUP/jgpu.c" \
    -o build/native/test_gpu || FAIL=1
gcc -O2 -Wall -DJUP_HOST_BUILD -I"$JUP" \
    test_spr.c "$JUP/jspr.c" "$JUP/jgpu.c" \
    -o build/native/test_spr || FAIL=1
gcc -O2 -Wall -DJUP_HOST_BUILD -I"$JUP" \
    test_layer.c \
    -o build/native/test_layer || FAIL=1
gcc -O2 -Wall -DJUP_HOST_BUILD -I"$JUP" \
    test_audio.c "$JUP/jaudio.c" \
    -o build/native/test_audio || FAIL=1
(cd build/native && ./test_render && ./test_render2 && ./test_fb \
    && ./test_gpu && ./test_spr && ./test_layer && ./test_audio) \
    | tee native.log || FAIL=1

echo
echo "=== 2. big-endian SPARC tests under qemu ==="
mkdir -p build/sparc
sparc64-linux-gnu-gcc -O2 -Wall -static -DJUP_HOST_BUILD -I"$JUP" \
    test_render.c "$JUP/jnes.c" "$JUP/jgb.c" "$JUP/jrgb2yuv.c" \
    -o build/sparc/test_render || FAIL=1
sparc64-linux-gnu-gcc -O2 -Wall -static -DJUP_HOST_BUILD -I"$JUP" \
    test_render2.c "$JUP/jsnes.c" "$JUP/jgen.c" \
    -o build/sparc/test_render2 || FAIL=1
sparc64-linux-gnu-gcc -O2 -Wall -static -DJUP_HOST_BUILD -I"$JUP" \
    test_fb.c "$JUP/jfb.c" "$JUP/jrgb2yuv.c" \
    -o build/sparc/test_fb || FAIL=1
sparc64-linux-gnu-gcc -O2 -Wall -static -DJUP_HOST_BUILD -I"$JUP" \
    test_gpu.c "$JUP/jgpu.c" \
    -o build/sparc/test_gpu || FAIL=1
sparc64-linux-gnu-gcc -O2 -Wall -static -DJUP_HOST_BUILD -I"$JUP" \
    test_spr.c "$JUP/jspr.c" "$JUP/jgpu.c" \
    -o build/sparc/test_spr || FAIL=1
sparc64-linux-gnu-gcc -O2 -Wall -static -DJUP_HOST_BUILD -I"$JUP" \
    test_layer.c \
    -o build/sparc/test_layer || FAIL=1
sparc64-linux-gnu-gcc -O2 -Wall -static -DJUP_HOST_BUILD -I"$JUP" \
    test_audio.c "$JUP/jaudio.c" \
    -o build/sparc/test_audio || FAIL=1
(cd build/sparc && qemu-sparc64 ./test_render && qemu-sparc64 ./test_render2 \
    && qemu-sparc64 ./test_fb && qemu-sparc64 ./test_gpu \
    && qemu-sparc64 ./test_spr && qemu-sparc64 ./test_layer \
    && qemu-sparc64 ./test_audio) | tee sparc.log || FAIL=1

echo
echo "=== 3. endianness cross-check ==="
grep '^CRC' native.log | sort > native.crc
grep '^CRC' sparc.log | sort > sparc.crc
if diff -u native.crc sparc.crc; then
    echo "CRCs identical on little- and big-endian — endian-clean."
else
    echo "FAIL: CRCs differ between endiannesses"
    FAIL=1
fi

echo
echo "=== 4. firmware compile-check (SPARC V8 ILP32, real headers) ==="
FWCC="sparc64-linux-gnu-gcc -m32 -mcpu=v8 -msoft-float -Wall -Wno-comment \
      -Wno-endif-labels -fsigned-char -c -I$REPO -I$JUP -Iecos_stub"
mkdir -p build/fw
for f in jnes jgb jsnes jgen jaudio jrgb2yuv jfb jcodec_ct952 jgpu jgpu_ct952 jspr jlayer_ct952 jshim_ct952 japp; do
    if $FWCC "$JUP/$f.c" -o "build/fw/$f.o" 2> "build/fw/$f.err"; then
        echo "  OK   $f.c"
    else
        echo "  FAIL $f.c"
        cat "build/fw/$f.err"
        FAIL=1
    fi
done

echo
if [ "$FAIL" -eq 0 ]; then
    echo "ALL CHECKS PASSED"
else
    echo "CHECKS FAILED"
fi
exit $FAIL
