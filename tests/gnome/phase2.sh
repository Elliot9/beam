#!/bin/bash
# Phase 2: window switching. Install the extension from Beam, "log in again" (restart the
# shell), then switch / close windows from the switcher and from the root search.
OUT=${OUT_ROOT:-/tmp/beam-gnome-tests}/out2
source "$(dirname "$0")/common.sh"
init_env
FAILS=0; n=0
mkdir -p $XDG_DATA_HOME/gnome-shell/extensions $XDG_CONFIG_HOME/beam
cp -r $HERE/beam-test@local $XDG_DATA_HOME/gnome-shell/extensions/
gsettings set org.gnome.shell enabled-extensions "['beam-test@local']"
echo '{"windowSwitcherHotkey": "Super+W"}' > $XDG_CONFIG_HOME/beam/settings.json
start_shell
python3 $HERE/testwin.py "Alpha Window" >/dev/null 2>&1 &
sleep 1.2
BEAM_DEBUG=1 $BEAM --background > $OUT/beam1.log 2>&1 &
sleep 2
keys alt+space 1.0
typ "switch windows"; keys Return 0.8
state windows-no-extension
shot windows-no-extension
check "switcher offers to install the extension" "grep -q '啟用視窗切換' $OUT/state-windows-no-extension.txt"
keys Return 1.5
state installed
# A copy only Beam wrote needs a new login; the package's system-wide copy may load right away.
if [ -f /usr/share/gnome-shell/extensions/beam-windows@local.beam/extension.js ]; then
    check "install enables the extension" "grep -q -E '視窗切換已啟用|登出再登入' $OUT/state-installed.txt"
else
    check "install asks for a new login" "grep -q '登出再登入' $OUT/state-installed.txt"
fi
check "extension files present (user copy, or the package's system copy)" "[ -f $XDG_DATA_HOME/gnome-shell/extensions/beam-windows@local.beam/extension.js -o -f /usr/share/gnome-shell/extensions/beam-windows@local.beam/extension.js ]"
check "extension enabled" "gsettings get org.gnome.shell enabled-extensions | grep -q beam-windows@local.beam"
keys Escape; keys Escape

echo "### log out and in again"
$BEAM --quit; sleep 0.5
stop_shell
start_shell
sleep 3  # let the new session settle (IBus, keymap) before pressing shortcuts
echo "extensions: $(gnome-extensions list --enabled 2>/dev/null | tr '\n' ' ')"
python3 $HERE/testwin.py "Alpha Window" >/dev/null 2>&1 &
sleep 1.2
python3 $HERE/testwin.py "Beta Window" >/dev/null 2>&1 &
sleep 1.2
python3 $HERE/testwin.py "Gamma Window" >/dev/null 2>&1 &
sleep 1.5
echo "focus: $(focused)"
echo "D-Bus List from another program (must be refused): $(gdbus call --session --dest org.gnome.Shell --object-path /local/beam/Windows --method local.beam.Windows.List 2>&1 | cut -c1-160)"
check "other programs cannot list windows" "gdbus call --session --dest org.gnome.Shell --object-path /local/beam/Windows --method local.beam.Windows.List 2>&1 | grep -q AccessDenied"
BEAM_DEBUG=1 $BEAM --background > $OUT/beam2.log 2>&1 &
sleep 2

keys alt+space 1.0
if ! grep -q 'visible=1' <($BEAM --dump-state); then
    echo "DIAG Alt+Space after re-login did not open Beam; launches=$(grep -c keyring $OUT/gsd.log) overview=$(js 'Main.overview.visible') modal=$(js 'Main.modalCount') actionMode=$(js 'Main.actionMode')"
    keys super+space 1.0
    echo "DIAG after super+space: inputsource popup? modal=$(js 'Main.modalCount')"; keys Escape
fi
typ "switch windows"; keys Return 0.8
state switcher
shot switcher
check "switcher lists the three windows" "grep -c 'Window' $OUT/state-switcher.txt | grep -q 3"
check "previous window pre-selected (like Alt+Tab)" "grep -q ' > Beta Window' $OUT/state-switcher.txt"
keys Return 1.0
check "Enter switches to Beta" "focused | grep -q Beta"
check "Beam hid itself" "grep -q 'visible=0' <($BEAM --dump-state)"

# the window-switcher hotkey (Super+W -> beam --windows)
echo "beam-windows binding: $(gsettings get org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/beam-windows/ binding)"
keys super+w 1.0
state super-w
check "Super+W opens the switcher" "grep -q 'mode=windows' $OUT/state-super-w.txt && grep -q 'active=1' $OUT/state-super-w.txt"
typ "gam"
state filtered
check "typing filters windows" "grep -q ' > Gamma Window' $OUT/state-filtered.txt"
keys ctrl+w 1.0
state closed
check "Ctrl+W closes Gamma" "! js 'global.display.get_tab_list(Meta.TabList.NORMAL_ALL, null).map(w => w.get_title()).join()' | grep -q Gamma"
keys Escape; keys Escape

# windows in the root search
keys alt+space 1.0
keys ctrl+a; typ "alpha"
state root-window
check "root search finds the window" "grep -q 'Alpha Window' $OUT/state-root-window.txt"
keys Return 1.0
check "Enter on a window result switches to it" "focused | grep -q Alpha"

# empty state lists open windows
keys alt+space 1.0
keys ctrl+a; keys BackSpace
state empty
shot empty
check "empty state shows open windows" "grep -q '已開啟的視窗' $OUT/state-empty.txt"
keys Escape
$BEAM --quit; sleep 0.5
stop_shell
echo "=== beam2 log"; grep -v "dump-state" $OUT/beam2.log | tail -20
echo "FAILS=$FAILS"
