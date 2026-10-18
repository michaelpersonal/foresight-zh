#!/bin/zsh
# Builds the Chinese-capable ASR bundle from the Gemma 4 E2B model Foresight downloads (models/15).
#
#   LITERT_VENV  venv with `pip install litert-lm-api litert-lm`  (default: ~/local-llm/venvs/litert)
#   OUT          output .litertlm  (default: ~/Library/Application Support/farsight/zh_asr_gemma4e2b_v3.litertlm)
#
# Quit Foresight first if it is running with the shim: it memory-maps the output file.
set -e
VENV=${LITERT_VENV:-$HOME/local-llm/venvs/litert}
OUT="${OUT:-$HOME/Library/Application Support/farsight/zh_asr_gemma4e2b_v3.litertlm}"
MODELS="$HOME/Library/Application Support/com.google.AIEdgeForesight/models"
SRC=$(ls "$MODELS"/15/*/*.litertlm 2>/dev/null | head -1)
[[ -f "$SRC" ]] || { echo "Gemma 4 E2B not found under $MODELS/15 - open Foresight once and let it finish downloading."; exit 1; }
[[ -x "$VENV/bin/litert-lm-builder" ]] || { echo "LiteRT-LM tools not found in $VENV"; exit 1; }

HERE=${0:A:h}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
echo "source: $SRC"
"$VENV/bin/litert-lm-peek" --litertlm_file "$SRC" --dump_files_dir "$WORK" >/dev/null
for f in $(grep -o 'Section[0-9]*_[A-Za-z0-9_]*\.[a-z]*' "$HERE/model.toml"); do
  [[ -f "$WORK/$f" ]] || { echo "missing section $f - the Gemma file layout changed; update model.toml"; exit 1; }
done
cp "$HERE/model.toml" "$HERE/LlmMetadataProto.pbtext" "$WORK/"
mkdir -p "${OUT:h}"
(cd "$WORK" && "$VENV/bin/litert-lm-builder" toml --path model.toml output --path "$OUT")
ls -la "$OUT"
