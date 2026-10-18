#!/bin/zsh
# Run by launchd every few seconds (see install-watcher.sh). If Foresight is running
# without the shim (plain launch from the Dock, a notification, a login item or an
# update), relaunches it through launch.sh.
#
# Pause:  touch ~/Library/Logs/farsight/watcher.paused   (rm to resume)
LOGDIR="$HOME/Library/Logs/farsight"
DIR=${0:A:h}
mkdir -p "$LOGDIR"
log() { print -r -- "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOGDIR/watcher.log"; }

[[ -e "$LOGDIR/watcher.paused" ]] && exit 0
[[ -f "$DIR/shim/libfarsight.dylib" ]] || exit 0
pid=$(pgrep -x Foresight | head -1)
[[ -z "$pid" ]] && exit 0
[[ "$(cat "$LOGDIR/shim.pid" 2>/dev/null)" == "$pid" ]] && exit 0

# Give a launch that is in progress (launch.sh itself) a moment to settle.
sleep 2
pid=$(pgrep -x Foresight | head -1)
[[ -z "$pid" || "$(cat "$LOGDIR/shim.pid" 2>/dev/null)" == "$pid" ]] && exit 0

# Rate limit: at most 3 relaunches in 5 minutes, so a broken shim cannot cause a restart loop.
stamps="$LOGDIR/watcher.relaunches"
now=$(date +%s)
recent=$(awk -v n="$now" '$1 > n - 300' "$stamps" 2>/dev/null | wc -l | tr -d ' ')
if (( recent >= 3 )); then
  log "Foresight (pid $pid) runs without the shim, but relaunched 3 times in 5 min - giving up for now"
  exit 0
fi
print "$now" >> "$stamps"
tail -n 20 "$stamps" > "$stamps.tmp" && mv "$stamps.tmp" "$stamps"
log "Foresight (pid $pid) started without the shim - relaunching with it"
"$DIR/launch.sh"
