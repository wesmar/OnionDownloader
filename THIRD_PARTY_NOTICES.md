# Third-party notices

Onion Downloader's own source code is licensed under the MIT License. The
distributed `od.exe` also contains a separately built Tor executable as a
compressed payload. Tor runs as a child process and retains its own license.

## Tor 0.4.9.11

- Project: https://gitlab.torproject.org/tpo/core/tor
- Source archive: https://dist.torproject.org/tor-0.4.9.11.tar.gz
- License: GNU General Public License version 3 or later
- Local configuration: Windows client only; relay, directory authority and
  directory cache modules disabled; proof-of-work client support retained.

The exact build configuration is recorded in [`build-tor.sh`](build-tor.sh).
That script downloads the corresponding source, builds it and replaces
`resources/tor.exe`.

### GPLv3 relationship and corresponding source

Onion Downloader's own code is licensed under the MIT License and does not link
Tor's code: the embedded Tor is aggregated as a compressed data payload and run
as a **separate child process**, so the two programs remain independent works
under the GPLv3's aggregation clause. This is why `od.exe` can be MIT while the
Tor it carries stays GPLv3-or-later.

For the GPLv3 requirement to accompany the distributed binary with corresponding
source, the complete source for the embedded Tor is the upstream release listed
above (`tor-0.4.9.11.tar.gz`), together with the exact, reproducible build
recipe in [`build-tor.sh`](build-tor.sh); the OpenSSL, libevent and zlib sources
are the pinned upstream archives that same script downloads and verifies.

## OpenSSL 3.5.7

- Project: https://www.openssl.org/
- License: Apache License 2.0
- Statically linked into the embedded Tor executable with unused algorithms,
  protocols, applications, tests and documentation disabled.

## libevent 2.1.12-stable

- Project: https://libevent.org/
- License: 3-clause BSD license
- Statically linked into the embedded Tor executable.

## zlib 1.3.1

- Project: https://zlib.net/
- License: zlib License
- Statically linked into the embedded Tor executable.

The authoritative license texts are included in the respective upstream
source archives. No third-party DLL is required at runtime.

