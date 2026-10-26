#!/bin/bash
# Builds the files for a GitHub release into dist/ (from the version in CMakeLists.txt):
#   beam-launcher_<version>_amd64.deb   Linux package (unit tests run first)
#   Beam-<version>-windows-x64.zip      Beam.exe + beam-plugin.exe, cross-compiled with MinGW-w64
#   SHA256SUMS.txt                      checksums of both
# Needs: build-essential cmake ninja-build libgtk-4-dev g++-mingw-w64-x86-64-posix zip
#   packaging/build-release.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^project(Beam VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
DIST=$ROOT/dist
cd "$ROOT"
rm -rf "$DIST"
mkdir -p "$DIST"

echo "== Linux $VERSION"
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBEAM_BUILD_TESTS=ON >/dev/null
cmake --build build-linux
build-linux/beam_tests
build-linux/beam_plugin_tests
packaging/linux/build-deb.sh build-linux "$DIST"

echo "== Windows $VERSION"
cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake >/dev/null
cmake --build build-win
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir "$STAGE/Beam"
cp build-win/Beam.exe build-win/beam-plugin.exe README.md README.zh-TW.md PLUGINS.md LICENSE "$STAGE/Beam/"
(cd "$STAGE" && zip -qr "$DIST/Beam-$VERSION-windows-x64.zip" Beam)

(cd "$DIST" && sha256sum ./*.deb ./*.zip | sed 's| \./| |' > SHA256SUMS.txt)
echo "== dist/"
ls -l "$DIST"
cat "$DIST/SHA256SUMS.txt"
