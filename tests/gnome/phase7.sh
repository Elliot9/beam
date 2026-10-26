#!/bin/bash
# Phase 7: GNOME Shell refuses to grab Beam's shortcut (the keys still belong to another shortcut at
# that moment). gsd-media-keys never retries by itself; Beam must notice the refusal (journal) and
# make it grab again: when Beam starts, from beam --doctor, and right after Beam registers.
OUT=${OUT_ROOT:-/tmp/beam-gnome-tests}/out7
source "$(dirname "$0")/common.sh"
init_env
FAILS=0; n=0
mkdir -p $XDG_DATA_HOME/gnome-shell/extensions
cp -r $HERE/beam-test@local $XDG_DATA_HOME/gnome-shell/extensions/
gsettings set org.gnome.shell enabled-extensions "['beam-test@local']"
start_shell
K=org.gnome.settings-daemon.plugins.media-keys
P=/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings
WM=org.gnome.desktop.wm.keybindings
BLOCK=$K.custom-keybinding:$P/blocker/
refusals() { grep -c "Failed to grab accelerator for keybinding custom:$P/beam-launcher/" $OUT/gsd.log; }
notes() { js "Main.messageTray.getSources().map(s => s.notifications.map(n => n.title + ': ' + n.body).join(' | ')).join(' ;; ')"; }
running() { $BEAM --dump-state >/dev/null 2>&1; }
# The state the user's machine was in: Beam's shortcut listed, its grab refused, the keys free again.
break_grab() {
    gsettings set $K custom-keybindings "@as []"; sleep 0.8                   # gsd lets go of the keys
    gsettings reset $WM activate-window-menu; sleep 1.2                       # the window menu takes them
    local before=$(refusals)
    gsettings set $K custom-keybindings "['$P/beam-launcher/']"; sleep 1.2      # gsd asks, the shell refuses
    gsettings set $WM activate-window-menu "@as []"; sleep 1.2                # keys free, nobody grabs them
    [ $(refusals) -gt $before ]
}
python3 $HERE/testwin.py "Alpha Window" >/dev/null 2>&1 &
sleep 1.5

BEAM_DEBUG=1 $BEAM --background > $OUT/beam.log 2>&1 &
for i in $(seq 40); do grep -q 'visible=1' <($BEAM --dump-state 2>/dev/null) && break; sleep 0.25; done
sleep 4
keys Escape 0.5
$BEAM --quit; sleep 1

check "the refusal can be reproduced" "break_grab"
keys alt+space 2.0
check "after a refusal Alt+Space does nothing" "! running"

# 1. when Beam starts
BEAM_DEBUG=1 $BEAM --background >> $OUT/beam.log 2>&1 &
sleep 5
keys alt+space 1.2
check "Beam's start makes GNOME grab the shortcut again" "grep -q 'visible=1 active=1' <($BEAM --dump-state)"
check "... and logs it" "grep -q 'refused Beam.s shortcut earlier in this login' $OUT/beam.log"
keys Escape 0.5

# 2. beam --doctor
check "the refusal can be reproduced again" "break_grab"
keys alt+space 1.2
check "Alt+Space does nothing again" "grep -q 'visible=0' <($BEAM --dump-state)"
$BEAM --doctor > $OUT/doctor-heal.txt 2>&1; echo $? > $OUT/doctor-heal.rc
echo "--- doctor heal (exit $(cat $OUT/doctor-heal.rc))"; cat $OUT/doctor-heal.txt
check "doctor sees the refusal and asks again" "grep -q '拒絕過註冊 Alt+Space，正在請它重新註冊' $OUT/doctor-heal.txt && grep -q '✓ GNOME 這次接受了 Alt+Space' $OUT/doctor-heal.txt && [ \$(cat $OUT/doctor-heal.rc) = 0 ]"
keys alt+space 1.2
check "Alt+Space works after the doctor" "grep -q 'visible=1 active=1' <($BEAM --dump-state)"
keys Escape 0.5
$BEAM --quit; sleep 1

# 3. right after registering: refused at first (the keys are still taken), accepted on the retry
gsettings set $BLOCK name Blocker; gsettings set $BLOCK command true; gsettings set $BLOCK binding '<Alt>space'
gsettings set $K custom-keybindings "['$P/blocker/']"; sleep 1.2
rm -rf $XDG_CONFIG_HOME/beam; gsettings reset $WM activate-window-menu
before=$(refusals)
BEAM_DEBUG=1 $BEAM --background >> $OUT/beam.log 2>&1 &
for i in $(seq 60); do [ $(refusals) -gt $before ] && break; sleep 0.1; done
check "first registration refused while another shortcut has the keys" "[ \$(refusals) -gt $before ]"
gsettings set $K custom-keybindings "['$P/beam-launcher/']"   # the other shortcut goes away
sleep 5
keys Escape 0.5
keys alt+space 1.2
check "Beam asked again and got the shortcut" "grep -q 'visible=1 active=1' <($BEAM --dump-state) && grep -q 'accepted Beam.s shortcut on attempt 2' $OUT/beam.log"
keys Escape 0.5
$BEAM --quit; sleep 1

# 4. refused for good: Beam stops after three tries and says so
gsettings set $K custom-keybindings "['$P/blocker/']"; sleep 1.2
BEAM_DEBUG=1 $BEAM --background >> $OUT/beam.log 2>&1 &
sleep 9
echo "notifications: $(notes)"
check "Beam gives up after three refusals and tells" "notes | grep -q '快捷鍵沒有生效: GNOME 拒絕註冊 Alt+Space'"
$BEAM --doctor > $OUT/doctor-blocked.txt 2>&1
echo "--- doctor blocked"; cat $OUT/doctor-blocked.txt
check "doctor reports the refusal and the other shortcut" "grep -q 'GNOME 拒絕註冊 Alt+Space（試了 3 次）' $OUT/doctor-blocked.txt && grep -q '自訂快捷鍵「Blocker」' $OUT/doctor-blocked.txt"
gsettings set $K custom-keybindings "['$P/beam-launcher/']"; sleep 1.2

# 5. an app holds Alt+Space through the global-shortcuts portal (approved in GNOME's dialog)
G=org.gnome.settings-daemon.global-shortcuts
gsettings set $G applications "['com.example.Grabby']"
gsettings set $G.application:/org/gnome/settings-daemon/global-shortcuts/com.example.Grabby/ shortcuts \
    "[('toggle', {'description': <'Show Grabby'>, 'shortcuts': <['<Alt>space']>})]"
gsettings set $K custom-keybindings "@as []"; sleep 1
python3 $HERE/grabber.py org.gnome.Settings '<Alt>space' > $OUT/grabber.log 2>&1 &
GRAB=$!
sleep 1.2
gsettings set $K custom-keybindings "['$P/beam-launcher/']"; sleep 1.2
$BEAM --doctor > $OUT/doctor-app.txt 2>&1
echo "--- doctor app"; cat $OUT/doctor-app.txt
check "doctor names the app's global shortcut" "grep -q 'App「com.example.Grabby」的全域快捷鍵「Show Grabby」' $OUT/doctor-app.txt"

# 6. held by a program that left no trace in GNOME's settings: the doctor hands over a probe
gsettings reset $G applications
$BEAM --doctor > $OUT/doctor-probe.txt 2>&1
echo "--- doctor probe"; cat $OUT/doctor-probe.txt
probe=$(grep '^(() =>' $OUT/doctor-probe.txt)
answer=$(js "$probe")
echo "probe answer: $answer"
check "doctor prints a Looking Glass probe when nothing explains it" "grep -q 'lg' $OUT/doctor-probe.txt && [ -n \"\$probe\" ]"
check "the probe names the program holding Alt+Space" "echo \"\$answer\" | grep -q '程式 python3 .*grabber.py'"
kill $GRAB; wait $GRAB 2>/dev/null; sleep 1

# 7. an extension whose schema id does not start with org.gnome.shell.extensions
EXT=$XDG_DATA_HOME/gnome-shell/extensions/launcher@example.org
mkdir -p $EXT/schemas
echo '{"uuid": "launcher@example.org", "name": "Example Launcher", "settings-schema": "org.example.launcher", "shell-version": ["50"]}' > $EXT/metadata.json
cat > $EXT/schemas/org.example.launcher.gschema.xml <<'XML'
<schemalist><schema id="org.example.launcher" path="/org/example/launcher/">
<key name="open-launcher" type="as"><default><![CDATA[['<Alt>space']]]></default></key></schema></schemalist>
XML
glib-compile-schemas $EXT/schemas
$BEAM --doctor > $OUT/doctor-ext.txt 2>&1
check "doctor names an extension by its name" "grep -q 'GNOME 擴充功能「Example Launcher」的「open-launcher」快捷鍵' $OUT/doctor-ext.txt"
rm -rf $EXT
$BEAM --quit; sleep 0.5
stop_shell
echo "=== beam log"; grep -vE '^beam\[[0-9]+\]: (focus|Show|Hide|Toggle)' $OUT/beam.log
echo "FAILS=$FAILS"
