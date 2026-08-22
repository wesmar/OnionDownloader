#!/usr/bin/env bash
set -euo pipefail

# Native Windows ARM64 (aarch64) client-only Tor build, cross-compiled under WSL
# with llvm-mingw (clang). Mirrors build-tor.sh but targets aarch64-w64-mingw32
# and adds a custom OpenSSL target because upstream ships no mingw-arm64 target.

WORK="${WORK:-/home/root/od-arm64}"
TOOLCHAIN="$(cat "$WORK/toolchain.path")"
export PATH="${TOOLCHAIN}bin:$PATH"
TARGET=aarch64-w64-mingw32
JOBS="${JOBS:-$(nproc)}"
REPO_DIR="${REPO_DIR:-/mnt/c/Projekty/od}"

TOR_VERSION=0.4.9.11
OPENSSL_VERSION=3.5.7
LIBEVENT_VERSION=2.1.12-stable
ZLIB_VERSION=1.3.1
TOR_SHA256=2e6c1720118c812acf0079fd47cf91b6bfaba5d766c321c4d3d2a28d6a11a8ed

SRC="$WORK/src"; BUILD="$WORK/build"
PREFIX="$WORK/prefix"; OPENSSL_PREFIX="$WORK/openssl-prefix"
mkdir -p "$SRC" "$BUILD" "$PREFIX/include" "$PREFIX/lib" "$OPENSSL_PREFIX" "$WORK/out"

need(){ command -v "$1" >/dev/null || { echo "Missing tool: $1" >&2; exit 1; }; }
for t in curl tar make perl sha256sum "$TARGET-clang" "$TARGET-ar" "$TARGET-strip"; do need "$t"; done

download(){ [ -f "$2" ] || curl --fail -L --retry 3 -o "$2" "$1"; }
extract(){ rm -rf "$2"; mkdir -p "$2"; tar -xf "$1" --strip-components=1 -C "$2"; }

echo "==> [$(date +%T)] toolchain: $($TARGET-clang --version | head -1)"

echo "==> [$(date +%T)] downloading pinned sources"
download "https://dist.torproject.org/tor-${TOR_VERSION}.tar.gz" "$SRC/tor.tar.gz"
echo "$TOR_SHA256  $SRC/tor.tar.gz" | sha256sum --check --status || { echo "Tor checksum mismatch"; exit 1; }
download "https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz" "$SRC/openssl.tar.gz"
download "https://github.com/libevent/libevent/releases/download/release-${LIBEVENT_VERSION}/libevent-${LIBEVENT_VERSION}.tar.gz" "$SRC/libevent.tar.gz"
download "https://zlib.net/fossils/zlib-${ZLIB_VERSION}.tar.gz" "$SRC/zlib.tar.gz"

echo "==> [$(date +%T)] building zlib ${ZLIB_VERSION}"
extract "$SRC/zlib.tar.gz" "$BUILD/zlib"
make -C "$BUILD/zlib" -f win32/Makefile.gcc clean >/dev/null 2>&1 || true
make -C "$BUILD/zlib" -f win32/Makefile.gcc -j"$JOBS" \
    PREFIX="${TARGET}-" CC="${TARGET}-clang" AR="${TARGET}-ar" RANLIB="${TARGET}-ranlib"
cp "$BUILD/zlib/zlib.h" "$BUILD/zlib/zconf.h" "$PREFIX/include/"
cp "$BUILD/zlib/libz.a" "$PREFIX/lib/"

echo "==> [$(date +%T)] building libevent ${LIBEVENT_VERSION}"
extract "$SRC/libevent.tar.gz" "$BUILD/libevent"
# libevent 2.1.12 hard-downgrades _WIN32_WINNT to 0x0501 (XP) right before
# <iphlpapi.h>, which leaves it inconsistent with NTDDI_VERSION under the newer
# mingw-w64 headers shipped by llvm-mingw (tcpmib.h then omits PMIB_TCPTABLE2 &c
# while iphlpapi.h still references them). Lift that hard-coded floor to 0x0601
# so the version macros agree. Harmless on the Windows-on-ARM (Win10+) target.
sed -i 's/#define _WIN32_WINNT 0x0501/#define _WIN32_WINNT 0x0601/g' "$BUILD/libevent/evutil.c"
(
    cd "$BUILD/libevent"
    ./configure --host="$TARGET" --prefix="$PREFIX" --disable-shared \
        --enable-static --disable-openssl --disable-samples \
        --disable-libevent-regress CC="${TARGET}-clang" \
        CFLAGS="-O2 -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000" \
        CPPFLAGS="-D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000"
    make -j"$JOBS"
    make install
)

echo "==> [$(date +%T)] building pruned OpenSSL ${OPENSSL_VERSION} (custom mingw-arm64 target)"
extract "$SRC/openssl.tar.gz" "$BUILD/openssl"
cat > "$BUILD/openssl/Configurations/50-mingw-arm64.conf" <<'CONF'
## Windows ARM64 (aarch64) target for llvm-mingw / clang.
## Inherits the generic mingw base but omits the x86-only "-m64" and x86_64 asm.
my %targets = (
    "mingw-arm64" => {
        inherit_from     => [ "mingw-common" ],
        sys_id           => "MINGW64",
        bn_ops           => add("SIXTY_FOUR_BIT"),
        uplink_arch      => undef,
        shared_extension => ".dll",
    },
);
CONF
(
    cd "$BUILD/openssl"
    ./Configure mingw-arm64 no-asm no-shared no-tests no-apps no-docs \
        no-argon2 no-aria no-async no-bf no-blake2 no-camellia no-capieng \
        no-cast no-cmp no-cms no-comp no-ct no-dgram no-dsa no-dso no-dtls \
        no-ec2m no-gost no-idea no-legacy no-md4 no-mdc2 no-ml-dsa no-ml-kem \
        no-module no-multiblock no-nextprotoneg no-ocb no-ocsp no-padlockeng \
        no-psk no-quic no-rc2 no-rc4 no-rfc3779 no-rmd160 no-seed no-siv \
        no-slh-dsa no-sm2 no-sm3 no-sm4 no-srp no-srtp no-ssl-trace no-ts \
        no-ui-console no-whirlpool no-winstore no-zlib no-zstd \
        --cross-compile-prefix="${TARGET}-" --prefix="$OPENSSL_PREFIX" \
        -O2 -ffunction-sections -fdata-sections
    make -j"$JOBS"
    make install_sw
)

echo "==> [$(date +%T)] building client-only Tor ${TOR_VERSION}"
extract "$SRC/tor.tar.gz" "$BUILD/tor"
(
    cd "$BUILD/tor"
    ./configure --host="$TARGET" --enable-gpl --disable-module-relay \
        --disable-module-dirauth --disable-unittests --disable-asciidoc \
        --disable-manpage --disable-html-manual --disable-system-torrc \
        --disable-gcc-hardening --enable-static-tor --disable-tool-name-check \
        --disable-lzma --disable-zstd \
        --with-openssl-dir="$OPENSSL_PREFIX" --with-libevent-dir="$PREFIX" \
        --with-zlib-dir="$PREFIX" CC="${TARGET}-clang" \
        CFLAGS="-O2 -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 -DNTDDI_VERSION=0x06010000 -ffunction-sections -fdata-sections" \
        LDFLAGS="-static -Wl,--gc-sections -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va"
    make -j"$JOBS" src/app/tor.exe
)

TOR_EXE="$BUILD/tor/src/app/tor.exe"
"${TARGET}-strip" --strip-all "$TOR_EXE"
cp "$TOR_EXE" "$WORK/out/tor-arm64.exe"

echo "==> [$(date +%T)] RESULT"
ls -l "$WORK/out/tor-arm64.exe"
sha256sum "$WORK/out/tor-arm64.exe"
echo "PE machine check:"; "$TARGET-objdump" -f "$WORK/out/tor-arm64.exe" 2>/dev/null | grep -Ei "file format|architecture" || true
