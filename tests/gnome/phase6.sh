#!/bin/bash
# Phase 6: `beam --doctor` and shortcut conflicts: before the first start, after it, with Beam's
# shortcut removed, with another custom shortcut or an extension on Alt+Space, with the window
# menu back on Alt+Space, without gsd-media-keys.
OUT=${OUT_ROOT:-/tmp/beam-gnome-tests}/out6
source "$(dirname "$0")/common.sh"
init_env
FAILS=0; n=0
mkdir -p $XDG_DATA_HOME/gnome-shell/extensions
cp -r $HERE/beam-test@local $XDG_DATA_HOME/gnome-shell/extensions/
gsettings set org.gnome.shell enabled-extensions "['beam-test@local']"
start_shell
K=org.gnome.settings-daemon.plugins.media-keys
P=/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings
OTHER=$K.custom-keybinding:$P/custom0/
doctor() {
    $BEAM --doctor > $OUT/doctor-$1.txt 2>&1; echo $? > $OUT/doctor-$1.rc
    echo "--- doctor $1 (exit $(cat $OUT/doctor-$1.rc))"; cat $OUT/doctor-$1.txt
}
notes() { js "Main.messageTray.getSources().map(s => s.notifications.map(n => n.title + ': ' + n.body).join(' | ')).join(' ;; ')"; }
python3 $HERE/testwin.py "Alpha Window" >/dev/null 2>&1 &
sleep 1.5

doctor fresh
check "before the first start: tells to open Beam" "grep -q '還沒開過 Beam' $OUT/doctor-fresh.txt && [ \$(cat $OUT/doctor-fresh.rc) = 1 ]"
check "before the first start: writes nothing" "[ \"\$(gsettings get $K custom-keybindings)\" = '@as []' ] && [ ! -e $XDG_CONFIG_HOME/beam/settings.json ]"

BEAM_DEBUG=1 $BEAM --background > $OUT/beam.log 2>&1 &
for i in $(seq 40); do grep -q 'visible=1' <($BEAM --dump-state 2>/dev/null) && break; sleep 0.25; done  # first start (cold caches: slow)
sleep 1
keys Escape 0.5
doctor ok
check "after the first start: all fine" "[ \$(cat $OUT/doctor-ok.rc) = 0 ] && grep -q '✓ GNOME 自訂快捷鍵「Beam」：Alt+Space' $OUT/doctor-ok.txt"

# Beam's shortcut deleted (for example in GNOME Settings)
gsettings set $K custom-keybindings "@as []"
sleep 1
keys alt+space 1.0
check "without its shortcut Alt+Space does nothing" "grep -q 'visible=0' <($BEAM --dump-state)"
doctor repair
check "doctor puts the shortcut back" "grep -q '已修正 GNOME 自訂快捷鍵' $OUT/doctor-repair.txt && gsettings get $K custom-keybindings | grep -q beam-launcher"
sleep 1
keys alt+space 1.0
check "Alt+Space works again" "grep -q 'visible=1 active=1' <($BEAM --dump-state)"
keys Escape 0.5

# another custom shortcut on Alt+Space
gsettings set $OTHER name 'Ulauncher'; gsettings set $OTHER command 'true'; gsettings set $OTHER binding '<Alt>space'
gsettings set $K custom-keybindings "['$P/beam-launcher/', '$P/custom0/']"
sleep 0.5
doctor custom
check "doctor names the other custom shortcut" "grep -q '自訂快捷鍵「Ulauncher」' $OUT/doctor-custom.txt && [ \$(cat $OUT/doctor-custom.rc) = 1 ]"
$BEAM --quit; sleep 1
BEAM_DEBUG=1 $BEAM --background >> $OUT/beam.log 2>&1 &
sleep 2.5
echo "notifications: $(notes)"
check "Beam warns about it when it starts" "notes | grep -q '快捷鍵可能無法使用: 「Alt+Space」也被自訂快捷鍵「Ulauncher」使用'"
gsettings set $K custom-keybindings "['$P/beam-launcher/']"
for key in name command binding; do gsettings reset $OTHER $key; done
sleep 0.5

# an extension (own schemas folder) with a keybinding on Alt+Space
EXT=$XDG_DATA_HOME/gnome-shell/extensions/fake@test/schemas
mkdir -p $EXT
cat > $EXT/org.gnome.shell.extensions.fake-launcher.gschema.xml <<'XML'
<schemalist>
  <schema id="org.gnome.shell.extensions.fake-launcher" path="/org/gnome/shell/extensions/fake-launcher/">
    <key name="toggle-launcher" type="as"><default><![CDATA[['<Alt>space']]]></default></key>
    <key name="other" type="as"><default><![CDATA[['<Super>F9']]]></default></key>
  </schema>
</schemalist>
XML
glib-compile-schemas $EXT
doctor extension
check "doctor names the extension keybinding" "grep -q '擴充功能「fake@test」的「toggle-launcher」快捷鍵' $OUT/doctor-extension.txt && ! grep -q '「other」' $OUT/doctor-extension.txt"
rm -rf $XDG_DATA_HOME/gnome-shell/extensions/fake@test

# the window menu got Alt+Space back (reset in GNOME Settings)
gsettings reset org.gnome.desktop.wm.keybindings activate-window-menu
sleep 0.5
doctor window-menu
check "doctor frees the window menu again" "[ \"\$(gsettings get org.gnome.desktop.wm.keybindings activate-window-menu)\" = '@as []' ] && grep -q '視窗選單' $OUT/doctor-window-menu.txt"
sleep 1
keys alt+space 1.0
check "Alt+Space shows Beam after that" "grep -q 'visible=1 active=1' <($BEAM --dump-state)"
keys Escape 0.5

# gsd-media-keys gone: no custom shortcut works
kill $GPID 2>/dev/null; wait $GPID 2>/dev/null
doctor no-gsd
check "doctor notices gsd-media-keys is not running" "grep -q 'gsd-media-keys）沒有在執行' $OUT/doctor-no-gsd.txt && [ \$(cat $OUT/doctor-no-gsd.rc) = 1 ]"
$BEAM --quit; sleep 0.5

# not a GNOME desktop (GNOME's schemas installed, but no gsd-media-keys): WSL, SSH / tty, another desktop
XDG_CURRENT_DESKTOP= WSL_DISTRO_NAME=Ubuntu doctor wsl
check "doctor in WSL points to the Windows version" "grep -q '這是 WSL' $OUT/doctor-wsl.txt && [ \$(cat $OUT/doctor-wsl.rc) = 1 ]"
env -u XDG_CURRENT_DESKTOP -u WAYLAND_DISPLAY -u DISPLAY XDG_SESSION_TYPE=tty $BEAM --doctor > $OUT/doctor-tty.txt 2>&1
check "doctor in a text console asks for the desktop's terminal" "grep -q '文字模式（SSH 或 tty）' $OUT/doctor-tty.txt"
XDG_CURRENT_DESKTOP=KDE doctor kde
check "doctor on another desktop says it is not GNOME" "grep -q '這不是 GNOME 桌面' $OUT/doctor-kde.txt"
rm -rf $XDG_CONFIG_HOME/beam
gsettings set $K custom-keybindings "@as []"; gsettings reset org.gnome.desktop.wm.keybindings activate-window-menu
XDG_CURRENT_DESKTOP=KDE BEAM_DEBUG=1 $BEAM --background >> $OUT/beam.log 2>&1 &
sleep 2.5
echo "notifications: $(notes)"
check "first start on another desktop: says so instead of 'press Alt+Space'" "notes | grep -q '無法自動設定快捷鍵: 這不是 GNOME 桌面' && ! notes | grep -q 'Beam 已就緒'"
check "first start on another desktop: GNOME settings left alone" "[ \"\$(gsettings get $K custom-keybindings)\" = '@as []' ] && [ \"\$(gsettings get org.gnome.desktop.wm.keybindings activate-window-menu)\" = \"['<Alt>space']\" ]"
$BEAM --quit; sleep 0.5
stop_shell
echo "=== beam log"; cat $OUT/beam.log
echo "FAILS=$FAILS"
