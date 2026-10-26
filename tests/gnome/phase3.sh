#!/bin/bash
# Phase 3: Chinese input (IBus Chewing / 注音) inside Beam on Wayland. Keys that the input
# method needs (Enter to commit, Esc to cancel) must not trigger Beam's own actions.
OUT=${OUT_ROOT:-/tmp/beam-gnome-tests}/out3
source "$(dirname "$0")/common.sh"
init_env
FAILS=0; n=0
mkdir -p $XDG_DATA_HOME/gnome-shell/extensions $XDG_CONFIG_HOME/beam
cp -r $HERE/beam-test@local $XDG_DATA_HOME/gnome-shell/extensions/
gsettings set org.gnome.shell enabled-extensions "['beam-test@local']"
gsettings set org.gnome.desktop.input-sources sources "[('xkb', 'us'), ('ibus', 'chewing')]"
echo '{}' > $XDG_CONFIG_HOME/beam/settings.json
start_shell
sleep 2
python3 $HERE/testwin.py "Alpha Window" >/dev/null 2>&1 &
sleep 1.2
BEAM_DEBUG=1 $BEAM --background > $OUT/beam.log 2>&1 &
sleep 2
keys alt+space 1.0
for retry in 1 2 3; do
  grep -q 'visible=1' <($BEAM --dump-state) && break
  echo "Alt+Space #$retry did not open Beam (gsd launches: $(grep -c keyring $OUT/gsd.log)); focus=$(focused)"
  keys alt+space 1.5
done
echo "input sources: $(js 'global.display ? "ok" : "?"') ibus: $(pgrep -a ibus-daemon | head -1)"
keys super+space 1.5
echo "current source: $(gsettings get org.gnome.desktop.input-sources mru-sources)"
typ "su3cl3"; sleep 0.8
state composing
shot composing
check "composition is not in the query yet" "grep -q 'query=\"\"' $OUT/state-composing.txt"
keys Return 0.8
state committed
shot committed
check "Enter commits 你好 (and does not run an action)" "grep -q 'visible=1 active=1 mode=root query=\"你好\"' $OUT/state-committed.txt"
typ "ji3"; sleep 0.5
keys Escape 0.8
state cancelled
check "Esc cancels the composition, Beam stays open" "grep -q 'visible=1 active=1 mode=root query=\"你好\"' $OUT/state-cancelled.txt"
keys super+space 1.0   # back to English
keys Escape 0.5
check "Esc (no composition) clears the query" "grep -q 'query=\"\"' <($BEAM --dump-state)"
$BEAM --quit; sleep 0.5
stop_shell
echo "=== beam log"; grep -v dump-state $OUT/beam.log | tail
echo "FAILS=$FAILS"
