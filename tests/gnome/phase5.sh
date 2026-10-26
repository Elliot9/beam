#!/bin/bash
# Phase 5: the shortcut starts Beam when it is not running; autostart file; theme follows
# GNOME's dark style; beam --unregister undoes the desktop integration.
OUT=${OUT_ROOT:-/tmp/beam-gnome-tests}/out5
source "$(dirname "$0")/common.sh"
init_env
FAILS=0; n=0
mkdir -p $XDG_DATA_HOME/gnome-shell/extensions
cp -r $HERE/beam-test@local $XDG_DATA_HOME/gnome-shell/extensions/
gsettings set org.gnome.shell enabled-extensions "['beam-test@local']"
start_shell
python3 $HERE/testwin.py "Alpha Window" >/dev/null 2>&1 &
sleep 1.2
$BEAM --background > $OUT/beam1.log 2>&1 &     # first run: registers everything
sleep 2.5
keys Escape
check "autostart entry written" "grep -q 'beam --background' $XDG_CONFIG_HOME/autostart/local.beam.Launcher.desktop"
$BEAM --quit; sleep 1
check "beam quit" "! $BEAM --dump-state >/dev/null 2>&1"
keys alt+space 2.5
state started-by-shortcut
check "the shortcut starts Beam when it is not running" "grep -q 'visible=1 active=1' $OUT/state-started-by-shortcut.txt"
keys Escape
gsettings set org.gnome.desktop.interface color-scheme prefer-dark; sleep 1
keys alt+space 1.0
shot dark
keys Escape
$BEAM --unregister > $OUT/unregister.txt 2>&1; sleep 1
cat $OUT/unregister.txt
check "unregister quits Beam" "! $BEAM --dump-state >/dev/null 2>&1"
check "custom shortcut removed" "! gsettings get org.gnome.settings-daemon.plugins.media-keys custom-keybindings | grep -q beam"
check "window menu has Alt+Space again" "gsettings get org.gnome.desktop.wm.keybindings activate-window-menu | grep -q '<Alt>space'"
check "autostart removed" "[ ! -e $XDG_CONFIG_HOME/autostart/local.beam.Launcher.desktop ]"
check "extension disabled" "! gsettings get org.gnome.shell enabled-extensions | grep -q beam-windows"
keys alt+space 1.5
check "Alt+Space no longer starts Beam" "! $BEAM --dump-state >/dev/null 2>&1"
stop_shell
echo "FAILS=$FAILS"
