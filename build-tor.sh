#!/usr/bin/env bash
set -euo pipefail

# Reproducible client-only Tor build for Windows x64, executed under WSL.
# Override REPO_DIR or WORK_DIR when the checkout lives elsewhere.
REPO_DIR="${REPO_DIR:-/mnt/c/Projekty/github/od}"
WORK_DIR="${WORK_DIR:-/mnt/c/temp/od-tor-build}"
JOBS="${JOBS:-$(nproc)}"
TARGET=x86_64-w64-mingw32

TOR_VERSION=0.4.9.11
OPENSSL_VERSION=3.5.7
LIBEVENT_VERSION=2.1.12-stable
ZLIB_VERSION=1.3.1
TOR_SHA256=2e6c1720118c812acf0079fd47cf91b6bfaba5d766c321c4d3d2a28d6a11a8ed

SRC="$WORK_DIR/src"
BUILD="$WORK_DIR/build"
PREFIX="$WORK_DIR/prefix"
OPENSSL_PREFIX="$WORK_DIR/openssl-prefix"

need() { command -v "$1" >/dev/null || { echo "Missing tool: $1" >&2; exit 1; }; }
for tool in curl tar make perl sha256sum "$TARGET-gcc" "$TARGET-strip"; do need "$tool"; done

mkdir -p "$SRC" "$BUILD" "$PREFIX/include" "$PREFIX/lib" "$OPENSSL_PREFIX"
download() {
    local url=$1 out=$2
    [ -f "$out" ] || curl --fail --location --retry 3 --output "$out" "$url"
}
extract() {
    local archive=$1 destination=$2
    rm -rf "$destination"
    mkdir -p "$destination"
    tar -xf "$archive" --strip-components=1 -C "$destination"
}

echo "==> Downloading pinned upstream sources"
download "https://dist.torproject.org/tor-${TOR_VERSION}.tar.gz" "$SRC/tor.tar.gz"
echo "$TOR_SHA256  $SRC/tor.tar.gz" | sha256sum --check --status || {
    echo "Tor source checksum mismatch" >&2; exit 1;
}
download "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz" "$SRC/openssl.tar.gz"
download "https://github.com/libevent/libevent/releases/download/release-${LIBEVENT_VERSION}/libevent-${LIBEVENT_VERSION}.tar.gz" "$SRC/libevent.tar.gz"
download "https://zlib.net/fossils/zlib-${ZLIB_VERSION}.tar.gz" "$SRC/zlib.tar.gz"

echo "==> Building zlib ${ZLIB_VERSION}"
extract "$SRC/zlib.tar.gz" "$BUILD/zlib"
make -C "$BUILD/zlib" -f win32/Makefile.gcc clean >/dev/null 2>&1 || true
make -C "$BUILD/zlib" -f win32/Makefile.gcc -j"$JOBS" PREFIX="$TARGET-"
cp "$BUILD/zlib/zlib.h" "$BUILD/zlib/zconf.h" "$PREFIX/include/"
cp "$BUILD/zlib/libz.a" "$PREFIX/lib/"

echo "==> Building libevent ${LIBEVENT_VERSION}"
extract "$SRC/libevent.tar.gz" "$BUILD/libevent"
(
    cd "$BUILD/libevent"
    ./configure --host="$TARGET" --prefix="$PREFIX" --disable-shared \
        --enable-static --disable-openssl --disable-samples \
        --disable-libevent-regress CFLAGS=-O2 CPPFLAGS=-D_WIN32_WINNT=0x0601
    make -j"$JOBS"
    make install
)

echo "==> Building pruned OpenSSL ${OPENSSL_VERSION}"
extract "$SRC/openssl.tar.gz" "$BUILD/openssl"
(
    cd "$BUILD/openssl"
    perl ./Configure mingw64 no-shared no-tests no-apps no-docs \
        no-argon2 no-aria no-async no-bf no-blake2 no-camellia no-capieng \
        no-cast no-cmp no-cms no-comp no-ct no-dgram no-dsa no-dso no-dtls \
        no-ec2m no-gost no-idea no-legacy no-md4 no-mdc2 no-ml-dsa no-ml-kem \
        no-module no-multiblock no-nextprotoneg no-ocb no-ocsp no-padlockeng \
        no-psk no-quic no-rc2 no-rc4 no-rfc3779 no-rmd160 no-seed no-siv \
        no-slh-dsa no-sm2 no-sm3 no-sm4 no-srp no-srtp no-ssl-trace no-ts \
        no-ui-console no-whirlpool no-winstore no-zlib no-zstd \
        --cross-compile-prefix="$TARGET-" --prefix="$OPENSSL_PREFIX" \
        -O2 -ffunction-sections -fdata-sections
    make -j"$JOBS"
    make install_sw
)

echo "==> Building client-only Tor ${TOR_VERSION}"
extract "$SRC/tor.tar.gz" "$BUILD/tor"
(
    cd "$BUILD/tor"
    ./configure --host="$TARGET" --enable-gpl --disable-module-relay \
        --disable-module-dirauth --disable-unittests --disable-asciidoc \
        --disable-manpage --disable-html-manual --disable-system-torrc \
        --disable-gcc-hardening --enable-static-tor \
        --with-openssl-dir="$OPENSSL_PREFIX" --with-libevent-dir="$PREFIX" \
        --with-zlib-dir="$PREFIX" \
        CFLAGS="-O2 -D_WIN32_WINNT=0x0601 -ffunction-sections -fdata-sections" \
        LDFLAGS="-static -static-libgcc -Wl,--gc-sections -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va"
    make -j"$JOBS" src/app/tor.exe
)

TOR_EXE="$BUILD/tor/src/app/tor.exe"
"$TARGET-strip" --strip-all "$TOR_EXE"
cp "$TOR_EXE" "$REPO_DIR/resources/tor.exe"

echo "==> Result"
sha256sum "$REPO_DIR/resources/tor.exe"
"$REPO_DIR/resources/tor.exe" --list-modules || true

if command -v powershell.exe >/dev/null; then
    WIN_BUILD_SCRIPT=$(wslpath -w "$REPO_DIR/build.ps1")
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$WIN_BUILD_SCRIPT" -RepackTor
else
    echo "Run .\\build.ps1 -RepackTor in Windows PowerShell."
fi

