#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="${OO_PS4_TOOLCHAIN:?Set OO_PS4_TOOLCHAIN to OpenOrbis 0.5.4}"
deps="${PS4_DEPS:-$root/deps}"
mkdir -p "$deps"
deps="$(cd "$deps" && pwd)"
jobs="${JOBS:-4}"

checkout() {
    local target="$deps/$1" url="$2" ref="$3"
    if [[ ! -d "$target" ]]; then
        git clone --no-checkout "$url" "$target"
        git -C "$target" checkout --detach "$ref"
    fi
}
checkout ihslib-stream https://github.com/kxn/ihslib.git 04688d19951c68bedc4370234cc1752043632cb3
for video_patch in "$root"/patches/ihslib-*.patch; do
    if ! (cd "$deps/ihslib-stream" && git apply --reverse --check "$video_patch" 2>/dev/null); then
        (cd "$deps/ihslib-stream" && git apply "$video_patch")
    fi
done
checkout mbedtls https://github.com/Mbed-TLS/mbedtls.git 2fc8413bfcb51354c8e679141b17b3f1a5942561
checkout opus https://github.com/xiph/opus.git ddbe48383984d56acd9e1ab6a090c54ca6b735a6
checkout protobuf-c-1.5.2 https://github.com/protobuf-c/protobuf-c.git 4719fdd7760624388c2c5b9d6759eb6a47490626

ffmpeg="$deps/ffmpeg-ps4-simd"
if [[ -f "$ffmpeg/lib/libavcodec.a" && -f "$ffmpeg/lib/libavutil.a" ]]; then
    exit 0
fi
if ! command -v nasm >/dev/null; then
    echo "Install NASM to build the optimized PS4 H.264 decoder." >&2
    exit 1
fi
if [[ ! -f "$deps/ffmpeg.tar.xz" ]]; then
    curl -fL --retry 2 https://ffmpeg.org/releases/ffmpeg-6.1.4.tar.xz -o "$deps/ffmpeg.tar.xz.part"
    mv "$deps/ffmpeg.tar.xz.part" "$deps/ffmpeg.tar.xz"
fi
printf '%s  %s\n' a231e3d5742c44b1cdaebfb98ad7b6200d12763e0b6db9e1e2c5891f2c083a18 \
    "$deps/ffmpeg.tar.xz" | sha256sum -c
mkdir -p "$deps/ffmpeg-src"
tar xf "$deps/ffmpeg.tar.xz" -C "$deps/ffmpeg-src" --strip-components=1
cd "$deps/ffmpeg-src"
./configure --prefix="$ffmpeg" --enable-cross-compile --arch=x86_64 --target-os=freebsd \
    --cc=clang --cxx=clang++ --ld="$root/scripts/ffmpeg-link.sh" --ar=ar --ranlib=ranlib --nm=nm --strip=strip \
    --extra-cflags="--target=x86_64-pc-freebsd12-elf -fPIC -isysroot $sdk -isystem $sdk/include -D__PS4__ -D__ORBIS__ -O2" \
    --disable-shared --enable-static --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swresample --disable-swscale --disable-postproc \
    --disable-network --disable-iconv --disable-zlib --disable-bzlib --disable-everything \
    --enable-avcodec --enable-avutil --enable-decoder=h264 --enable-parser=h264 \
    --x86asmexe=nasm --enable-x86asm --enable-pic --enable-pthreads --disable-w32threads --disable-os2threads
if ! grep -qx '#define HAVE_PTHREADS 1' config.h || ! grep -qx '#define HAVE_X86ASM 1' config.h; then
    echo "FFmpeg requires pthreads and x86 assembly for the PS4 build." >&2
    exit 1
fi
sed -i 's/^#define HAVE_SYSCTL 1/#define HAVE_SYSCTL 0/' config.h
make -j"$jobs"
make install
