#!/bin/bash
# Colour probe helper: launch Henge on the iPhone with the probe on, watch it live, ask for a capture, pull and read it.
# The probe itself is devices/pvg_colorprobe.h; this only drives it with Apple's own device tools
# (devicectl for launch/console/files, and libimobiledevice's idevicesyslog for the live stream).
#
#   probe.sh launch [secs]   relaunch Henge with HV_COLOR_PROBE=1 (secs: also capture every N seconds)
#   probe.sh watch           live, filtered stream of probe lines (idevicesyslog; Ctrl-C to stop)
#   probe.sh console [secs]  launch attached to Xcode's console path (devicectl --console): lines appear as they happen,
#                            and stopping this command stops Henge. Use `launch` + `watch` when the Mac must keep running.
#   probe.sh trigger         ask the running probe for one capture (pushes Documents/colorprobe/trigger)
#   probe.sh mode encoded|linear   switch how the half-float display is shown, live (encoded = pass-through, the fix;
#                            linear = the old sRGB encode, for ACES-like linear profiles). Logged as event=color_mode.
#   probe.sh follow [secs]   follow the probe lines by pulling Documents/henge.log every few seconds (default 5). Slower than watch but
#                            does not depend on the syslog relay, which can stall; henge.log is the authoritative record.
#   probe.sh capture         trigger, wait until the capture is done, pull the files, print the summary
#   probe.sh pull            copy every file in Documents/colorprobe from the phone to $COLORPROBE_OUT
#   probe.sh log             pull henge.log and keep only the colorprobe lines ($COLORPROBE_OUT/colorprobe.log)
#   probe.sh summary [dir]   analyse the latest pull (tools/colorprobe/analyze.py)
#   probe.sh status          is the phone reachable, is Henge running
# Environment: HENGE_DEVICE (udid), HENGE_BUNDLE, COLORPROBE_OUT (where pulls go).
set -u
DEV=${HENGE_DEVICE:-00008150-001A29A20A87801C}
BID=${HENGE_BUNDLE:-com.hungbenjamin402.henge}
OUT=${COLORPROBE_OUT:-$HOME/Library/Caches/henge-colorprobe}
HERE="$(cd "$(dirname "$0")" && pwd)"
REMOTE=Documents/colorprobe
mkdir -p "$OUT"

dc() { xcrun devicectl device "$@"; }
files() { dc info files --device "$DEV" --domain-type appDataContainer --domain-identifier "$BID" --subdirectory "$1" 2>/dev/null | awk 'NR>2 && $1 !~ /^-+$/ && NF {print $1}'; }

env_json() { # $1 = capture-every seconds (optional)
    local secs=${1:-0}
    printf '{"HV_COLOR_PROBE":"1","HV_COLOR_PROBE_MAX":"8","HV_COLOR_PROBE_SECS":"%s","HV_LOG_CONSOLE":"colorprobe"}' "$secs"
}

latest_pull() { ls -dt "$OUT"/pull-* 2>/dev/null | head -1; }

cmd=${1:-help}; shift || true
case "$cmd" in
launch)
    dc process launch --device "$DEV" --terminate-existing --environment-variables "$(env_json "${1:-0}")" "$BID" 2>&1 | tail -2
    echo "probe is on. Next: probe.sh watch   (and probe.sh capture when the screen shows what you want measured)"
    ;;
watch)
    # idevicesyslog -m keeps only lines containing the text; the sed strips the syslog prefix so lines read as the probe wrote them.
    exec idevicesyslog -u "$DEV" -p Henge -m 'colorprobe: ' | sed -u 's/^.*\(colorprobe: v=\)/\1/'
    ;;
console)
    exec xcrun devicectl device process launch --device "$DEV" --terminate-existing --console \
        --environment-variables "$(env_json "${1:-0}")" "$BID"
    ;;
trigger)
    tmp=$(mktemp -t colorprobe-trigger) && : > "$tmp"
    dc copy to --device "$DEV" --domain-type appDataContainer --domain-identifier "$BID" --source "$tmp" --destination "$REMOTE/trigger" 2>&1 | tail -1
    rm -f "$tmp"
    ;;
capture)
    before=$(files "$REMOTE" | grep -c -- '-post\.raw$')
    "$0" trigger
    echo -n "waiting for the capture"
    for i in $(seq 1 40); do
        sleep 1.5; echo -n .
        now=$(files "$REMOTE" | grep -c -- '-post\.raw$')
        if [ "$now" -gt "$before" ]; then echo " done"; break; fi
        if [ "$i" = 40 ]; then echo " timed out (is the Mac showing a picture? is the probe on? try: probe.sh status)"; exit 1; fi
    done
    sleep 1 # let the last file finish writing
    "$0" pull && "$0" summary
    ;;
pull)
    dest="$OUT/pull-$(date +%H%M%S)"; mkdir -p "$dest"
    n=0
    for f in $(files "$REMOTE"); do
        case "$f" in trigger) continue;; esac
        dc copy from --device "$DEV" --domain-type appDataContainer --domain-identifier "$BID" --source "$REMOTE/$f" --destination "$dest/$f" >/dev/null 2>&1 && n=$((n+1))
    done
    echo "pulled $n files to $dest"
    ;;
log)
    dc copy from --device "$DEV" --domain-type appDataContainer --domain-identifier "$BID" --source Documents/henge.log --destination "$OUT/henge.log" >/dev/null 2>&1 \
        || { echo "could not pull henge.log"; exit 1; }
    grep 'colorprobe: v=' "$OUT/henge.log" | sed 's/^.*\(colorprobe: v=\)/\1/' > "$OUT/colorprobe.log"
    echo "$(wc -l < "$OUT/colorprobe.log") probe lines in $OUT/colorprobe.log"
    ;;
summary)
    d=${1:-$(latest_pull)}
    [ -n "$d" ] || { echo "nothing pulled yet: probe.sh pull"; exit 1; }
    exec python3 -I "$HERE/analyze.py" "$d" --regions
    ;;
mode)
    m=${1:-}
    case "$m" in encoded|linear) ;; *) echo "usage: probe.sh mode encoded|linear"; exit 2;; esac
    tmp=$(mktemp -t colorprobe-mode) && : > "$tmp"
    dc copy to --device "$DEV" --domain-type appDataContainer --domain-identifier "$BID" --source "$tmp" --destination "$REMOTE/mode-$m" 2>&1 | tail -1
    rm -f "$tmp"; echo "asked for mode $m (the probe applies it within a second; look for event=color_mode)"
    ;;
follow)
    every=${1:-5}; seen=0; f="$OUT/follow.log"
    echo "following $REMOTE-side probe lines every ${every}s (Ctrl-C to stop)"
    while true; do
        if dc copy from --device "$DEV" --domain-type appDataContainer --domain-identifier "$BID" --source Documents/henge.log --destination "$f" >/dev/null 2>&1; then
            total=$(grep -c 'colorprobe: v=' "$f")
            if [ "$total" -lt "$seen" ]; then seen=0; fi   # a new run: the log started over
            if [ "$total" -gt "$seen" ]; then grep 'colorprobe: v=' "$f" | sed 's/^.*\(colorprobe: v=\)/\1/' | tail -n +$((seen+1)); seen=$total; fi
        fi
        sleep "$every"
    done
    ;;
status)
    xcrun devicectl list devices 2>&1 | grep -i "$DEV" | head -1
    if dc info processes --device "$DEV" 2>/dev/null | grep -q "Henge.app"; then echo "Henge: running"; else echo "Henge: not running (or the developer connection is unavailable)"; fi
    echo "files on the phone:"; files "$REMOTE" | sed 's/^/  /'
    ;;
*)
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    ;;
esac
