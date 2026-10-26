#!/bin/bash
# Runs every phase in its own D-Bus session. Needs: gnome-shell, gnome-settings-daemon,
# dconf, python3-gi + gir1.2-gtk-4.0, ibus-chewing (phase 3), a system D-Bus, and no logind
# seats (/run/systemd/seats absent) so GNOME Shell uses its dummy login manager.
# Meant for a throw-away container or chroot: it replaces HOME and GNOME settings.
# Full output of each phase: $OUT_ROOT/<phase>.txt (default /tmp/beam-gnome-tests).
# Arguments: the phases to run (default: all).
HERE=$(cd "$(dirname "$0")" && pwd)
export OUT_ROOT=${OUT_ROOT:-/tmp/beam-gnome-tests}
mkdir -p "$OUT_ROOT"
status=0
phases=${*:-phase1 phase2 phase3 phase4 phase5 phase6 phase7}
for phase in $phases; do
    echo "===== $phase"
    dbus-run-session -- "$HERE/$phase.sh" > "$OUT_ROOT/$phase.txt" 2>&1
    grep -E '^(PASS|FAIL|INFO)|FAILS=' "$OUT_ROOT/$phase.txt"
    grep -q '^FAILS=0$' "$OUT_ROOT/$phase.txt" || status=1
done
exit $status
