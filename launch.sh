#!/bin/zsh
# Launches Foresight with the farsight (Persian) transcription shim. Restarts it if it is already running.
# FARSIGHT_TRACE=1 launch.sh  -> log every ODML call and transcript result to ~/Library/Logs/farsight/shim.log
# UI language: Persian (right-to-left) by default. FARSIGHT_LANG=en launch.sh for English,
#              FARSIGHT_LANG=system to follow the macOS language.
# Summary headings: Persian by default; FARSIGHT_HEADINGS=en launch.sh keeps "Overview"/"Details".
# Font: Persian text uses Vazirmatn when installed; FARSIGHT_FONT=system launch.sh keeps the macOS font.
SHIM="${0:A:h}/shim/libfarsight.dylib"
if pgrep -x Foresight >/dev/null; then
  osascript -e 'tell application "Foresight" to quit' >/dev/null 2>&1
  for _ in {1..20}; do pgrep -x Foresight >/dev/null || break; sleep 0.5; done
  pkill -x Foresight 2>/dev/null; sleep 1
fi
env_args=(--env "DYLD_INSERT_LIBRARIES=$SHIM")
[[ -n "$FARSIGHT_TRACE" ]] && env_args+=(--env "FARSIGHT_TRACE=$FARSIGHT_TRACE")
[[ -n "$FARSIGHT_FONT" ]] && env_args+=(--env "FARSIGHT_FONT=$FARSIGHT_FONT")
[[ -n "$FARSIGHT_HEADINGS" ]] && env_args+=(--env "FARSIGHT_HEADINGS=$FARSIGHT_HEADINGS")
[[ -n "$FARSIGHT_GEMMA_SAMPLING" ]] && env_args+=(--env "FARSIGHT_GEMMA_SAMPLING=$FARSIGHT_GEMMA_SAMPLING")
lang=${FARSIGHT_LANG:-fa}
app_args=()
case $lang in
  system) ;;
  fa) app_args=(--args -AppleLanguages "(fa)" -AppleLocale fa_IR) ;;
  *) app_args=(--args -AppleLanguages "($lang)") ;;
esac
exec open "${env_args[@]}" -a Foresight "${app_args[@]}"
