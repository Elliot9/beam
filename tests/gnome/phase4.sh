#!/bin/bash
# Phase 4: launching apps / commands from Beam on Wayland (focus goes to the new window),
# clipboard, terminal commands, a plugin.
OUT=${OUT_ROOT:-/tmp/beam-gnome-tests}/out4
source "$(dirname "$0")/common.sh"
init_env
FAILS=0; n=0
mkdir -p $XDG_DATA_HOME/gnome-shell/extensions $XDG_CONFIG_HOME/beam $XDG_DATA_HOME/applications
cp -r $HERE/beam-test@local $XDG_DATA_HOME/gnome-shell/extensions/
gsettings set org.gnome.shell enabled-extensions "['beam-test@local']"
echo '{}' > $XDG_CONFIG_HOME/beam/settings.json
cat > $XDG_DATA_HOME/applications/delta-test.desktop <<DESKTOP
[Desktop Entry]
Type=Application
Name=Delta Test App
GenericName=Window for tests
Exec=python3 $HERE/testwin.py "Delta Window"
Icon=utilities-terminal
StartupNotify=true
Actions=second;

[Desktop Action second]
Name=Open Epsilon
Exec=python3 $HERE/testwin.py "Epsilon Window"
DESKTOP
start_shell
python3 $HERE/testwin.py "Alpha Window" >/dev/null 2>&1 &
sleep 1.2
BEAM_DEBUG=1 $BEAM --background > $OUT/beam.log 2>&1 &
sleep 2.5

keys alt+space 1.0
typ "delta"
state app-search
shot app-search
check "desktop app found" "grep -q ' > Delta Test App' $OUT/state-app-search.txt"
keys Return 2.5
echo "focus after launch: $(focused)"
check "launched app window has the focus" "focused | grep -q 'Delta Window'"
check "Beam hid" "grep -q 'visible=0' <($BEAM --dump-state)"

keys alt+space 1.0
keys ctrl+a; typ "delta"
keys ctrl+k
state app-actions
check "desktop actions listed" "grep -q 'Open Epsilon' $OUT/state-app-actions.txt"
keys Down; keys Return 2.5
check "desktop action launched and focused" "focused | grep -q 'Epsilon Window'"

# calculator result to the clipboard
keys alt+space 1.0
keys ctrl+a; typ "6*7"; keys Return 0.8
CLIP=$(gdbus call --session --dest org.gnome.Shell --object-path /local/beam/Test --method local.beam.Test.Clipboard)
echo "clipboard: $CLIP"
check "Enter copied 42" "echo \"$CLIP\" | grep -q \"'42'\""

# shell command in a terminal (Ptyxis)
keys alt+space 1.0
keys ctrl+a; typ "> echo beam-was-here"
state shell-cmd
keys Return 4
echo "focus after terminal command: $(focused)"
check "terminal window opened with the focus" "! focused | grep -q -E 'Beam|none|Epsilon'"

# a Shell plugin end to end
keys alt+space 1.0
keys ctrl+a; typ "newplugin demo"; keys Down; keys Down; keys Return 1.5
keys alt+space 1.0
keys ctrl+a; typ "demo hi"; sleep 1
state plugin
check "plugin answers" "grep -q 'HI' $OUT/state-plugin.txt"
shot plugin
keys Escape; keys Escape; keys Escape
$BEAM --quit; sleep 0.5
stop_shell
echo "=== beam log"; grep -v dump-state $OUT/beam.log | grep -v "^beam\[" | tail
echo "FAILS=$FAILS"
