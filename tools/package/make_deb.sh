#!/bin/bash
# Build the Linux packages inside a container (needs Docker, for example via Colima on macOS):
#   saurcom_<v>_<arch>.deb                       Debian and Ubuntu
#   saurcom-<v>-Linux-<arch>.tar.gz              the same files, for unpacking by hand
#   saurcom-<v>-linux-<arch>-portable.tar.gz     everything it needs inside, for any Linux (no installation, run bin/saurcom)
# FFmpeg is built static into the program, so the packages do not depend on the FFmpeg version of the system they are installed on.
#   tools/package/make_deb.sh                       native architecture (arm64 on an Apple-silicon Mac); packages land in build-packages/
#   PLATFORM=linux/amd64 tools/package/make_deb.sh  the x86-64 packages (built under emulation: slow, but the result is a normal x86-64 binary)
set -e
cd "$(dirname "$0")/../.."
PLATFORM=${PLATFORM:-}
IMAGE=saurcom-linux-pkg; VOL=saurcom-pkg-build; FFV=saurcom-pkg-ffmpeg; PF=
if [ -n "$PLATFORM" ]; then IMAGE=saurcom-linux-pkg-x86; VOL=saurcom-pkg-build-x86; FFV=saurcom-pkg-ffmpeg-x86; PF="--platform $PLATFORM"; fi
if ! docker image inspect $IMAGE >/dev/null 2>&1; then
  if [ -n "$PLATFORM" ]; then
    # the legacy docker builder ignores --platform when the base image is cached for another architecture, so the x86 image is made by hand
    PKGS=$(tr '\n\\' '  ' < tools/dev/Dockerfile.package | grep -o 'install -y [^&]*' | sed 's/install -y //')
    docker rm -f saurcom-x86-setup >/dev/null 2>&1 || true
    docker run --platform $PLATFORM --name saurcom-x86-setup -e DEBIAN_FRONTEND=noninteractive ubuntu:22.04 bash -c "apt-get update && apt-get install -y $PKGS"
    docker commit saurcom-x86-setup $IMAGE >/dev/null && docker rm saurcom-x86-setup >/dev/null
  else
    docker build -t $IMAGE -f tools/dev/Dockerfile.package tools/dev
  fi
fi
mkdir -p build-packages
# the FFmpeg source: use a copy that is already on this computer when there is one, otherwise the container downloads it
FFSRC=
for f in build-windows-deps/src/ffmpeg-8.0.tar.xz tools/package/cache/ffmpeg-8.0.tar.xz; do [ -f "$f" ] && FFSRC="/src/$f" && break; done
# under emulation a compiler can run out of memory with many jobs at once: build FFmpeg with few
JOBS=; [ -n "$PLATFORM" ] && JOBS="-e FFMPEG_JOBS=4"
docker run --rm $PF $JOBS -v "$PWD":/src:ro -v $VOL:/build -v $FFV:/ffmpeg -v "$PWD/build-packages":/out -w /build $IMAGE bash -c '
  set -e
  [ -f /ffmpeg/lib/libavcodec.a ] || /src/tools/package/build_ffmpeg_static.sh /ffmpeg '"$FFSRC"'
  rm -rf /build/radiolibs; /src/tools/package/collect_radio_libs_linux.sh /build/radiolibs
  cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_APP=ON -DDECT2_BUILD_TESTS=ON -DDECT2_HAVE_HAMLIB=ON -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_SKIP_INSTALL_ALL_DEPENDENCY=ON -DDECT2_FFMPEG_PREFIX=/ffmpeg -DDECT2_RADIO_LIB_DIR=/build/radiolibs >/dev/null
  cmake --build /build -j$(nproc) --target dect2 dect2cli dect2scan 2>&1 | grep -E "error|Error" -A3 | head -30
  cpack -G "DEB;TGZ" 2>&1 | tail -4
  /src/tools/package/make_portable_linux.sh /build /out
  cp /build/saurcom_*.deb /build/saurcom-*.tar.gz /out/ 2>/dev/null; ls -la /out'
