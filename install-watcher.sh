#!/bin/zsh
# Installs (or with --uninstall removes) the LaunchAgent that keeps Foresight running with the shim.
LABEL=io.github.hadi-buddy.farsight.watcher
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
DIR=${0:A:h}
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null
if [[ "$1" == --uninstall ]]; then rm -f "$PLIST"; echo "watcher removed"; exit 0; fi
cat > "$PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>$DIR/watcher.sh</string></array>
  <key>StartInterval</key><integer>5</integer>
  <key>RunAtLoad</key><true/>
  <key>ProcessType</key><string>Background</string>
  <key>StandardErrorPath</key><string>$HOME/Library/Logs/farsight/watcher.err</string>
</dict>
</plist>
PLIST
launchctl bootstrap "gui/$(id -u)" "$PLIST" && echo "watcher installed: $PLIST"
