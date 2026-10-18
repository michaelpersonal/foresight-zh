#!/bin/zsh
# Downloads the Persian engine: sherpa-onnx C library, a Shenava Persian speech model
# (NeMo CTC, Apache-2.0) and Silero VAD. Persian is the default engine.
# Installs into ~/Library/Application Support/farsight.
#   FARSIGHT_MODEL=koochik (default, 114M params, more accurate, ~450 MB)
#   FARSIGHT_MODEL=rizeh   (32M params, smaller and faster, ~120 MB)
set -e
SHERPA_VERSION=1.13.8
DEST="${FARSIGHT_MODELS:-$HOME/Library/Application Support/farsight}"
REL=https://github.com/k2-fsa/sherpa-onnx/releases/download
case ${FARSIGHT_MODEL:-koochik} in
  rizeh) NAME=Rizeh ;;
  *) NAME=Koochik ;;
esac
HF=https://huggingface.co/Reza2kn/Shenava-$NAME-v1.0-sherpa-onnx/resolve/main
FA="$DEST/persian/shenava-${NAME:l}-v1.0"
mkdir -p "$DEST/sensevoice" "$DEST/sherpa-onnx" "$FA"

# Resumable download: keeps a .part file and continues from it after a dropped connection.
fetch() {  # fetch <url> <dest>
  [[ -s "$2" ]] && return 0
  local tries=0
  until curl -fL --progress-bar -C - --connect-timeout 20 --speed-limit 1024 --speed-time 60 \
      -o "$2.part" "$1"; do
    (( ++tries >= 50 )) && { echo "download failed: $1"; return 1; }
    echo "connection dropped - resuming ($tries)..."; sleep 3
  done
  mv "$2.part" "$2"
}

if [[ ! -f "$DEST/sherpa-onnx/lib/libsherpa-onnx-c-api.dylib" ]]; then
  echo "downloading sherpa-onnx $SHERPA_VERSION"
  ARCHIVE="$DEST/sherpa-onnx-v$SHERPA_VERSION.tar.bz2"
  fetch "$REL/v$SHERPA_VERSION/sherpa-onnx-v$SHERPA_VERSION-osx-arm64-shared-no-tts-lib.tar.bz2" "$ARCHIVE"
  tmp=$(mktemp -d)
  tar xjf "$ARCHIVE" -C "$tmp"
  rm -rf "$DEST/sherpa-onnx/lib" && mv "$tmp"/*/lib "$DEST/sherpa-onnx/lib" && rm -rf "$tmp" "$ARCHIVE"
fi
for f in model.onnx tokens.txt; do
  echo "downloading Shenava $NAME v1.0: $f"
  fetch "$HF/$f" "$FA/$f"
done
[[ -s "$FA/LICENSE" ]] || curl -fsL --max-time 30 -o "$FA/LICENSE" "$HF/LICENSE" || rm -f "$FA/LICENSE"
if [[ ! -f "$DEST/sensevoice/silero_vad.onnx" ]]; then
  echo "downloading Silero VAD"
  fetch "$REL/asr-models/silero_vad.onnx" "$DEST/sensevoice/silero_vad.onnx"
fi
# Vazirmatn (SIL OFL 1.1), used for Persian text in the app (shim/persian_font.c).
VZ=https://raw.githubusercontent.com/rastikerdar/vazirmatn/6e553e33489a8f9dfaccc76860a2e3f3c1e66de7
mkdir -p "$DEST/fonts"
echo "downloading Vazirmatn font"
for w in Thin ExtraLight Light Regular Medium SemiBold Bold ExtraBold Black; do
  fetch "$VZ/fonts/ttf/Vazirmatn-$w.ttf" "$DEST/fonts/Vazirmatn-$w.ttf"
done
[[ -s "$DEST/fonts/OFL.txt" ]] || curl -fsL --max-time 30 -o "$DEST/fonts/OFL.txt" "$VZ/OFL.txt" || true
rm -f "${0:A:h}/engine"  # no override: the shim defaults to persian
du -sh "$DEST"
