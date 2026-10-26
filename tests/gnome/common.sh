# Shared helpers for the Wayland (headless GNOME Shell 50) tests. See README.md.
# WARNING: HOME and XDG_RUNTIME_DIR are replaced by throw-away folders (they are wiped).
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export HOME=${BEAM_TEST_HOME:-/tmp/beam-gnome-home}
export XDG_RUNTIME_DIR=${BEAM_TEST_RUNTIME:-/tmp/beam-gnome-runtime}
export XDG_CONFIG_HOME=$HOME/.config XDG_DATA_HOME=$HOME/.local/share XDG_CACHE_HOME=$HOME/.cache
export LANG=C.UTF-8 XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=GNOME WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland
export NO_AT_BRIDGE=1 GTK_A11Y=none GSK_RENDERER=${GSK_RENDERER:-cairo}
BEAM=${BEAM:-$HERE/../../build-linux/beam}
OUT=${OUT:-/tmp/beam-gnome-tests/out}
# No journal here: Beam reads gsd-media-keys' messages through fakebin/journalctl (from gsd.log).
export PATH=$HERE/fakebin:$PATH BEAM_TEST_GSD_LOG=$OUT/gsd.log

# D-Bus-activated services (dconf-service!) must see the same HOME / XDG dirs as the clients.
init_env() {
    rm -rf $HOME $XDG_RUNTIME_DIR $OUT; mkdir -p $HOME $OUT; mkdir -p -m 700 $XDG_RUNTIME_DIR
    dbus-update-activation-environment HOME XDG_RUNTIME_DIR XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME LANG \
        XDG_SESSION_TYPE XDG_CURRENT_DESKTOP WAYLAND_DISPLAY GDK_BACKEND GSK_RENDERER NO_AT_BRIDGE GTK_A11Y
    # The virtual pointer starts at (0,0), the hot corner: it would open the overview at random.
    gsettings set org.gnome.desktop.interface enable-hot-corners false
}

T() { gdbus call --session --dest org.gnome.Shell --object-path /local/beam/Test --method "local.beam.Test.$1" "${@:2}" >/dev/null; }
js() { gdbus call --session --dest org.gnome.Shell --object-path /local/beam/Test --method local.beam.Test.Eval "$1" | sed -E "s/^\('(.*)',\)$/\1/"; }
keys() { T Keys "$1"; sleep ${2:-0.5}; }
typ() { T Type "$1"; sleep 0.6; }
shot() { n=$((n+1)); T Screenshot "$OUT/$(printf %02d $n)-$1.png"; }
focused() { js "global.display.focus_window ? global.display.focus_window.get_title() + ' (pid ' + global.display.focus_window.get_pid() + ')' : 'none'"; }
state() { $BEAM --dump-state > "$OUT/state-$1.txt" 2>&1; echo "--- $1  [focus: $(focused)]"; cat "$OUT/state-$1.txt"; }
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; FAILS=$((FAILS+1)); fi; }

start_shell() {
    for attempt in 1 2 3; do
        rm -f $XDG_RUNTIME_DIR/gnome-shell-disable-extensions
        env MUTTER_DEBUG=${SHELL_DEBUG:-} gnome-shell --headless --wayland --no-x11 --virtual-monitor 1280x800 >> $OUT/shell.log 2>&1 &
        SPID=$!
        for i in $(seq 60); do
            gdbus call --session --dest org.gnome.Shell --object-path /local/beam/Test --method local.beam.Test.Eval "1" >/dev/null 2>&1 && break
            sleep 0.25
        done
        js "Main.overview.hide()" >/dev/null  # the shell starts in the overview, which keeps the focus
        sleep 0.5
        # Now and then a headless session ignores every modifier shortcut (GNOME's own Alt+F2 too,
        # without Beam involved): check with Alt+F2 and start a new session if so.
        T Keys "alt+F2"; sleep 1
        if [ "$(js 'Main.modalCount')" != 0 ]; then
            T Keys Escape; sleep 0.5
            break
        fi
        echo "INFO shortcuts do not work in this headless session; starting GNOME Shell again"
        kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
        pkill -x ibus-daemon 2>/dev/null; pkill -x ibus-portal 2>/dev/null; pkill -f '^/usr/libexec/ibus-' 2>/dev/null
        sleep 1
    done
    env G_MESSAGES_DEBUG=${GSD_DEBUG:-} /usr/libexec/gsd-media-keys >> $OUT/gsd.log 2>&1 &
    GPID=$!
    sleep 1.5
}
stop_shell() {
    kill $GPID 2>/dev/null; kill $SPID 2>/dev/null
    wait $SPID 2>/dev/null; wait $GPID 2>/dev/null
    # IBus outlives the shell; a stale daemon on the same bus would confuse the next shell.
    pkill -x ibus-daemon 2>/dev/null; pkill -x ibus-portal 2>/dev/null; pkill -f '^/usr/libexec/ibus-' 2>/dev/null
    sleep 0.8
}
