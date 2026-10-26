#!/bin/bash
# Builds beam-launcher_<version>_<arch>.deb from a CMake build directory (configured with
# -DCMAKE_INSTALL_PREFIX=/usr and already built):
#   packaging/linux/build-deb.sh build-linux [output-dir]
set -euo pipefail
BUILD=${1:?usage: build-deb.sh <build-dir> [output-dir]}
OUT=$(cd "${2:-.}" && pwd)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VERSION=$(sed -n 's/^project(Beam VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
ARCH=$(dpkg --print-architecture)
STAGE=$(mktemp -d)
chmod 0755 "$STAGE"
WORK=$(mktemp -d)
trap 'rm -rf "$STAGE" "$WORK"' EXIT

DESTDIR="$STAGE" cmake --install "$BUILD" --prefix /usr --strip >/dev/null

# Shared-library dependencies of the binaries (dpkg-shlibdeps wants a debian/control).
mkdir -p "$WORK/debian"
printf 'Source: beam-launcher\n\nPackage: beam-launcher\nArchitecture: any\n' > "$WORK/debian/control"
DEPENDS=$(cd "$WORK" && dpkg-shlibdeps -O "$STAGE/usr/bin/beam" "$STAGE/usr/bin/beam-plugin" 2>/dev/null |
          sed -n 's/^shlibs:Depends=//p')

mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<CONTROL
Package: beam-launcher
Version: $VERSION
Architecture: $ARCH
Maintainer: Beam <beam@localhost>
Installed-Size: $(du -sk "$STAGE/usr" | cut -f1)
Depends: $DEPENDS
Recommends: gnome-shell, gnome-settings-daemon, python3, xdg-terminal-exec | ptyxis | gnome-terminal
Suggests: nodejs
Section: utils
Priority: optional
Description: Raycast-style launcher for GNOME
 Press Alt+Space to search applications, open windows, files, quicklinks,
 a calculator and built-in commands, and to switch windows. Favorites can be
 pinned and reordered, and plugins can be written in any language (JSON over
 stdin/stdout); beam-plugin creates and tests them.
 .
 On GNOME (Wayland) the hotkey is a GNOME custom shortcut and window switching
 uses the bundled "Beam Window Switcher" GNOME Shell extension.
CONTROL

# apt shows this: the hotkey only exists once Beam has run (it writes the GNOME shortcut then).
cat > "$STAGE/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    if [ -z "$2" ]; then
        cat <<'MSG'

Beam 已安裝。下一步：從應用程式清單開啟「Beam」一次（或在終端機執行 beam）。
Beam 第一次啟動時才會在 GNOME 加上 Alt+Space 快捷鍵，之後隨時按 Alt+Space 就能叫出來。
按了沒反應的話，執行 beam --doctor 檢查。

MSG
    elif pgrep -x beam >/dev/null 2>&1; then
        cat <<'MSG'

Beam 已更新。正在執行的 Beam 還是舊版：執行 beam --quit 後重新開啟 Beam（或登出再登入）。

MSG
    fi
fi
exit 0
POSTINST
chmod 0755 "$STAGE/DEBIAN/postinst"

dpkg-deb --root-owner-group --build "$STAGE" "$OUT/beam-launcher_${VERSION}_${ARCH}.deb" >/dev/null
echo "$OUT/beam-launcher_${VERSION}_${ARCH}.deb"
