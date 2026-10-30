#!/bin/bash
# Download the sources and prebuilt MSYS2 packages that cross_deps_windows.sh builds the Windows dependencies from, into build-windows-deps/src.
# Every file is checked: release archives by SHA-256, git sources by the commit their tag points to (the tree is then packed with git archive).
# A file already there with the right checksum is kept. Needs curl, git and shasum or sha256sum.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$ROOT/build-windows-deps/src
mkdir -p "$SRC/msys2"
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -c1-64; }

# <file> <sha256> <url>
archive() {
  local f=$1 want=$2 url=$3
  [ -f "$SRC/$f" ] && [ "$(sha "$SRC/$f")" = "$want" ] && return
  echo "fetch $f"
  local n=0
  until curl -fsSL --retry 5 --retry-all-errors --retry-delay 5 -C - -o "$SRC/$f.part" "$url"; do   # (-C -: a dropped connection resumes)
    n=$((n+1)); [ $n -lt 4 ] || { echo "error: could not download $url"; exit 1; }
    echo "retry $f"; sleep $((n*15))
  done
  local got; got=$(sha "$SRC/$f.part")
  [ "$got" = "$want" ] || { rm -f "$SRC/$f.part"; echo "error: $f has SHA-256 $got, expected $want"; exit 1; }
  mv "$SRC/$f.part" "$SRC/$f"
}

# <file> <top folder in the archive> <repository> <ref> <commit>: the tree of that commit as a .tar.gz
gitsrc() {
  local f=$1 top=$2 repo=$3 ref=$4 commit=$5 stamp="$SRC/$1.commit"
  [ -f "$SRC/$f" ] && [ "$(cat "$stamp" 2>/dev/null)" = "$commit" ] && return
  echo "fetch $f ($repo $ref)"
  local g; g=$(mktemp -d)
  git -C "$g" init -q
  git -C "$g" fetch -q --depth 1 "https://github.com/$repo" "$ref" || git -C "$g" fetch -q --depth 1 "https://github.com/$repo" "$commit"
  local got; got=$(git -C "$g" rev-parse 'FETCH_HEAD^{commit}')
  [ "$got" = "$commit" ] || { rm -rf "$g"; echo "error: $repo $ref is commit $got, expected $commit"; exit 1; }
  git -C "$g" archive --format=tar.gz --prefix="$top/" -o "$SRC/$f" "$commit"
  echo "$commit" > "$stamp"
  rm -rf "$g"
}

archive libusb-1.0.27.tar.bz2   ffaa41d741a8a3bee244ac8e54a72ea05bf2879663c098c82fc5757853441575 https://github.com/libusb/libusb/releases/download/v1.0.27/libusb-1.0.27.tar.bz2
archive glfw-3.4.zip            b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9 https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
archive hackrf-2024.02.1.tar.xz d9ced67e6b801cd02c18d0c4654ed18a4bcb36c24a64330c347dfccbd859ad16 https://github.com/greatscottgadgets/hackrf/releases/download/v2024.02.1/hackrf-2024.02.1.tar.xz
archive ffmpeg-8.0.tar.xz       b2751fccb6cc4c77708113cd78b561059b6fa904b24162fa0be2d60273d27b8e https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz
archive libxml2-2.12.9.tar.xz   59912db536ab56a3996489ea0299768c7bcffe57169f0235e7f962a91f483590 https://download.gnome.org/sources/libxml2/2.12/libxml2-2.12.9.tar.xz
archive hamlib-4.6.2.tar.gz     b2ac73f44dd1161e95fdee6c95276144757647bf92d7fdb369ee2fe41ed47ae8 https://github.com/Hamlib/Hamlib/releases/download/4.6.2/hamlib-4.6.2.tar.gz

gitsrc rtl-sdr-blog-1.3.6.tar.gz    rtl-sdr-blog-1.3.6             rtlsdrblog/rtl-sdr-blog     refs/tags/v1.3.6            240bd0e1e6d9f64361b6949047468958cd08aa31
gitsrc airspyone_host-1.0.10.tar.gz airspyone_host-1.0.10          airspy/airspyone_host       refs/tags/v1.0.10           229d5a5045b890621cd9cc200a19b1541c6021ae
gitsrc soapysdr-0.8.1.tar.gz        SoapySDR-soapy-sdr-0.8.1       pothosware/SoapySDR         refs/tags/soapy-sdr-0.8.1   1cf5a539a21414ff509ff7d0eedfc5fa8edb90c6
gitsrc soapyremote-0.5.3.tar.gz     SoapyRemote-soapy-remote-0.5.3 pothosware/SoapyRemote      refs/tags/soapy-remote-0.5.3 baa92793181cd93e28b3dc926b744de4f67d2283
gitsrc libiio-0.25.tar.gz           libiio-0.25                    analogdevicesinc/libiio     refs/tags/v0.25             b6028fdeef888ab45f7c1dd6e4ed9480ae4b55e3
gitsrc bladeRF-2023.02.tar.gz       bladeRF-2023.02                Nuand/bladeRF               refs/tags/2023.02           41ef63460956e833c9b321252245257ab3946055
gitsrc no-OS-0bba46e.tar.gz         no-OS-0bba46e                  analogdevicesinc/no-OS      0bba46e6f6f75785a65d425ece37d0a04daf6157 0bba46e6f6f75785a65d425ece37d0a04daf6157
gitsrc LimeSuite-23.11.0.tar.gz     LimeSuite-23.11.0              myriadrf/LimeSuite          refs/tags/v23.11.0          c2d9e87754586304577beff4dc5b16fb9b72c826
gitsrc airspyhf-1.6.8.tar.gz        airspyhf-1.6.8                 airspy/airspyhf             refs/tags/1.6.8             8891387edddcd185e2949e9814e9ef35f46f0722

# MSYS2 UCRT64: libusb-1.0.dll (shared by libairspyhf and the program) and the GCC runtime DLLs; checksums from the repository database
M=https://repo.msys2.org/mingw/ucrt64
for p in \
  "mingw-w64-ucrt-x86_64-libusb-1.0.30-1-any.pkg.tar.zst 20106e6ff31b4581bb111b28a68b6b79a50dc3e5fc3033944e1c4b3ff8cd812e" \
  "mingw-w64-ucrt-x86_64-libgcc-16.2.0-6-any.pkg.tar.zst 80b2497f019977dd9528b1abc8621b1328a0115ab1f724047ccbb56bf323f11d" \
  "mingw-w64-ucrt-x86_64-libstdc++-16.2.0-6-any.pkg.tar.zst 33c5e58a85a538cf843a8672991cce591f0231d66da3b66a57629712d64be17f" \
  "mingw-w64-ucrt-x86_64-libwinpthread-14.0.0.r569.g34eeafb7d-1-any.pkg.tar.zst b4ebe2826e848c5e790e06ea7b37d3e0594e005f42382b0506886f17df8127da"; do
  set -- $p
  archive "msys2/$1" "$2" "$M/$1"
done
echo "sources ready in $SRC"
