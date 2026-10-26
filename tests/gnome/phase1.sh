#!/bin/bash
# Phase 1: first run, GNOME custom shortcut -> beam --toggle -> focus; typing; Esc; click outside;
# launching an app from Beam hands over focus.
OUT=${OUT_ROOT:-/tmp/beam-gnome-tests}/out1
source "$(dirname "$0")/common.sh"
init_env
FAILS=0; n=0
mkdir -p $XDG_DATA_HOME/gnome-shell/extensions
cp -r $HERE/beam-test@local $XDG_DATA_HOME/gnome-shell/extensions/
gsettings set org.gnome.shell enabled-extensions "['beam-test@local']"
start_shell
python3 $HERE/testwin.py "Alpha Window" >/dev/null 2>&1 &
sleep 1.2
python3 $HERE/testwin.py "Beta Window" >/dev/null 2>&1 &
sleep 1.5
echo "focus before beam: $(focused)"
BEAM_DEBUG=1 $BEAM --background > $OUT/beam.log 2>&1 &
sleep 2.5
KB=org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/beam-launcher/
echo "custom-keybindings: $(gsettings get org.gnome.settings-daemon.plugins.media-keys custom-keybindings)"
echo "beam-launcher: $(gsettings get $KB binding) -> $(gsettings get $KB command)"
echo "window menu: $(gsettings get org.gnome.desktop.wm.keybindings activate-window-menu)"
check "shortcut registered" "[ \"\$(gsettings get $KB binding)\" = \"'<Alt>space'\" ]"
check "window menu shortcut released" "[ \"\$(gsettings get org.gnome.desktop.wm.keybindings activate-window-menu)\" = '@as []' ]"
state first-run
check "first run shows the launcher, focused" "grep -q 'visible=1 active=1' $OUT/state-first-run.txt"
shot first-run
keys Escape
check "Esc hides" "grep -q 'visible=0' <($BEAM --dump-state)"
check "focus back on Beta" "focused | grep -q Beta"

keys alt+space 1.0
state hotkey
check "Alt+Space shows Beam, focused" "grep -q 'visible=1 active=1' $OUT/state-hotkey.txt"
shot hotkey
typ "term"
state typed
check "typing reaches the search field" "grep -q 'query=\"term\"' $OUT/state-typed.txt"
shot typed
keys alt+space 1.0
check "Alt+Space again hides" "grep -q 'visible=0' <($BEAM --dump-state)"
keys alt+space 1.0
state reopened
check "reopen keeps the query" "grep -q 'visible=1 active=1 mode=root query=\"term\"' $OUT/state-reopened.txt"
typ "x"
check "typing replaces the kept query (it was selected)" "grep -q 'query=\"x\"' <($BEAM --dump-state)"
T Click 30.0 600.0 1; sleep 0.8
check "click outside the panel hides" "grep -q 'visible=0' <($BEAM --dump-state)"

# Super (overview) takes focus away -> hide
keys alt+space 1.0
js "Main.overview.show()" >/dev/null; sleep 2.5
check "the overview (keyboard gone for good) hides Beam" "grep -q 'visible=0' <($BEAM --dump-state)"
js "Main.overview.hide()" >/dev/null; sleep 1

# launch an app from Beam: the new window must get the focus (activation token)
keys alt+space 1.0
keys ctrl+a; typ "beta"; sleep 0.3
state beta-search
keys Escape; keys Escape 0.8
echo "focus at end: $(focused)"
echo "extensions enabled on first run: $(gsettings get org.gnome.shell enabled-extensions)"
check "first run enabled the window extension" "gsettings get org.gnome.shell enabled-extensions | grep -q beam-windows"
$BEAM --quit; sleep 0.5
stop_shell
echo "=== beam log"; cat $OUT/beam.log
echo "FAILS=$FAILS"
