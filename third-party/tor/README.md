# Embedded Tor — build transparency

This folder exists so that anyone can independently confirm **exactly what the
Tor engine inside `od.exe` is, how it was built, and that nothing else was
slipped in**. The bundled transport is an ordinary, unmodified, client-only
build of upstream Tor — not a fork, not a patched binary, and not a repackaged
Tor Browser. Everything here is reproducible from public sources.

The bundled engine is **not** shipped as a loose `tor.exe`. It lives as an
LZX-21 compressed cabinet appended to the application icon - `resources/app.ico`
for the x64 engine, `resources/app-arm64.ico` for the native ARM64 engine - is
expanded at runtime with Windows' own `cabinet.dll`, and is run as a separate
child process. It is never statically linked into Onion Downloader's own code.
Both engines are the same client-only Tor from the same pinned sources; they
differ only in target architecture and toolchain (see *Native ARM64* below).

## What is bundled

| Component | Version | Upstream |
|---|---|---|
| Tor (client only) | 0.4.9.11 | https://dist.torproject.org/tor-0.4.9.11.tar.gz |
| OpenSSL | 3.5.7 | https://github.com/openssl/openssl |
| libevent | 2.1.12-stable | https://github.com/libevent/libevent |
| zlib | 1.3.1 | https://zlib.net/ |

The pinned Tor source checksum is in
[`verification/tor-0.4.9.11.tar.gz.sha256`](verification/tor-0.4.9.11.tar.gz.sha256)
and is re-checked by the build script before anything is compiled.

## The canonical build script

The single source of truth is [`../../build-tor.sh`](../../build-tor.sh) in the
repository root. It runs under WSL (Ubuntu) and cross-compiles a **native
Windows x64 PE** with MinGW-w64 — Linux is only the build host, not the target.
The commands below are copied from that script and match the real `config.log`
of the shipped binary verbatim.

### zlib

```
make -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32-
```

### libevent

```
./configure --host=x86_64-w64-mingw32 --prefix=$PREFIX --disable-shared \
    --enable-static --disable-openssl --disable-samples \
    --disable-libevent-regress CFLAGS=-O2 CPPFLAGS=-D_WIN32_WINNT=0x0601
```

### OpenSSL (pruned)

```
perl ./Configure mingw64 no-shared no-tests no-apps no-docs \
    no-argon2 no-aria no-async no-bf no-blake2 no-camellia no-capieng \
    no-cast no-cmp no-cms no-comp no-ct no-dgram no-dsa no-dso no-dtls \
    no-ec2m no-gost no-idea no-legacy no-md4 no-mdc2 no-ml-dsa no-ml-kem \
    no-module no-multiblock no-nextprotoneg no-ocb no-ocsp no-padlockeng \
    no-psk no-quic no-rc2 no-rc4 no-rfc3779 no-rmd160 no-seed no-siv \
    no-slh-dsa no-sm2 no-sm3 no-sm4 no-srp no-srtp no-ssl-trace no-ts \
    no-ui-console no-whirlpool no-winstore no-zlib no-zstd \
    --cross-compile-prefix=x86_64-w64-mingw32- --prefix=$OPENSSL_PREFIX \
    -O2 -ffunction-sections -fdata-sections
```

### Tor (client only)

```
./configure --host=x86_64-w64-mingw32 --enable-gpl --disable-module-relay \
    --disable-module-dirauth --disable-unittests --disable-asciidoc \
    --disable-manpage --disable-html-manual --disable-system-torrc \
    --disable-gcc-hardening --enable-static-tor \
    --with-openssl-dir=$OPENSSL_PREFIX --with-libevent-dir=$PREFIX \
    --with-zlib-dir=$PREFIX \
    CFLAGS="-O2 -D_WIN32_WINNT=0x0601 -ffunction-sections -fdata-sections" \
    LDFLAGS="-static -static-libgcc -Wl,--gc-sections -Wl,--nxcompat \
             -Wl,--dynamicbase -Wl,--high-entropy-va"
```

### Native ARM64 (AArch64)

The ARM64 engine is the same client-only Tor, built by
[`../../build-tor-arm64.sh`](../../build-tor-arm64.sh). The stock MinGW that
ships with Ubuntu only targets x86-64 Windows, so it cross-compiles with
**llvm-mingw** (clang, `aarch64-w64-mingw32`), and OpenSSL is built through a
small custom `mingw-arm64` target because upstream ships none - only the x86
`mingw` / `mingw64` targets. The client-only flag set is identical to x64; the
only differences are:

- the toolchain (llvm-mingw / clang instead of MinGW-w64 / gcc);
- the custom `mingw-arm64` OpenSSL target with `no-asm`;
- `--disable-lzma --disable-zstd` on Tor (the cross toolchain carries no
  target-side lzma/zstd, exactly like the x64 engine, which also reports both as
  N/A);
- a one-line patch to libevent 2.1.12's `evutil.c` lifting its hard-coded
  `_WIN32_WINNT 0x0501` to `0x0601`, so it agrees with the newer mingw-w64
  headers.

The result is a genuine native `IMAGE_FILE_MACHINE_ARM64` PE. On Windows on ARM
nothing runs under x64 emulation.

## What is disabled, and why

| Removed | Reason |
|---|---|
| Relay module (`--disable-module-relay`) | A downloader is a Tor **client**; it never carries traffic for others. |
| Directory authority (`--disable-module-dirauth`) | It never operates network authority infrastructure. |
| Directory cache role | It is a client, not a public cache. |
| Unit tests, man pages, HTML manual | Build-time material, not runtime functionality. |
| System-wide `torrc` (`--disable-system-torrc`) | Each managed process gets its own explicit private configuration; no machine-wide file is read. |
| Most OpenSSL algorithms, apps, tests, docs, legacy provider | The client uses only a small set of modern primitives; everything else is cut before compilation. |

**Proof-of-work is deliberately kept enabled** (`pow: yes`). Removing it would
save a little space but break access to Onion services that require PoW under
load — a smaller engine that cannot reach a defended service is not equivalent.

`--disable-gcc-hardening` turns off GCC's bulky default hardening path; the PE
still receives real exploit mitigations through the linker flags above
(`--nxcompat`, `--dynamicbase`, `--high-entropy-va`), i.e. NX/DEP, ASLR and
high-entropy ASLR.

## Reproduce it yourself

On Ubuntu under WSL, install the standard build tools, Perl, autotools and
MinGW-w64, then from the repository root:

```bash
./build-tor.sh
```

The script downloads the pinned sources, verifies the Tor checksum, builds all
four components, strips the result, writes `resources/tor.exe`, prints its
SHA-256 and module list, and finally asks `build.ps1 -RepackTor` to refresh the
compressed payload inside `resources/app.ico`.

## Verify the result

The [`verification/`](verification/) folder records the exact output of the
shipped engine so a reviewer can compare against a fresh build:

| File | Check |
|---|---|
| [`tor-version.txt`](verification/tor-version.txt) | `tor.exe --version` — versions of Tor, OpenSSL, libevent, zlib and the compiler |
| [`tor-list-modules.txt`](verification/tor-list-modules.txt) | `tor.exe --list-modules` — `relay: no`, `dirauth: no`, `dircache: no`, `pow: yes` |
| [`tor-imports.txt`](verification/tor-imports.txt) | Imported DLLs — only Windows system libraries; OpenSSL/libevent/zlib are statically linked in |
| [`tor-sha256.txt`](verification/tor-sha256.txt) | SHA-256 of the built `tor.exe` (the payload expanded from `app.ico` at runtime) |

For the **native ARM64** engine - its `--version` and `--list-modules` only run
on Windows on ARM, so those are checked natively rather than captured here,
while the static facts are recorded the same way:

| File | Check |
|---|---|
| [`tor-arm64-machine.txt`](verification/tor-arm64-machine.txt) | PE machine type - `IMAGE_FILE_MACHINE_ARM64 (0xAA64)`, format `COFF-ARM64` |
| [`tor-arm64-imports.txt`](verification/tor-arm64-imports.txt) | Imported DLLs - only Windows system libraries plus the always-present UCRT (`api-ms-win-crt-*`) |
| [`tor-arm64-sha256.txt`](verification/tor-arm64-sha256.txt) | SHA-256 of the native ARM64 `tor.exe` (the payload inside `app-arm64.ico`) |

Regenerate them from any `tor.exe`:

```bash
tor.exe --version
tor.exe --list-modules
# Imports (MSVC): dumpbin /dependents tor.exe
# Imports (MinGW): x86_64-w64-mingw32-objdump -p tor.exe | grep "DLL Name"
sha256sum tor.exe
```

The imports list containing only `ADVAPI32`, `IPHLPAPI`, `KERNEL32`, `msvcrt`,
`SHELL32`, `SHLWAPI`, `USER32` and `WS2_32` is the key point: there is no
third-party DLL dependency and nothing is loaded from outside Windows itself.

## License

The embedded Tor is **GPLv3-or-later** and remains a separate program from
Onion Downloader's MIT-licensed code. Full licensing and the corresponding-source
statement are in [`../../THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md).
