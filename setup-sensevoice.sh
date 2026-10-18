#!/bin/zsh
# Downloads the SenseVoice engine: sherpa-onnx C library, SenseVoice-Small (int8) and Silero VAD.
# Installs into ~/Library/Application Support/farsight (about 270MB).
set -e
SHERPA_VERSION=1.13.8
DEST="${FARSIGHT_MODELS:-$HOME/Library/Application Support/farsight}"
REL=https://github.com/k2-fsa/sherpa-onnx/releases/download
mkdir -p "$DEST/sensevoice" "$DEST/sherpa-onnx"

if [[ ! -f "$DEST/sherpa-onnx/lib/libsherpa-onnx-c-api.dylib" ]]; then
  echo "downloading sherpa-onnx $SHERPA_VERSION"
  tmp=$(mktemp -d)
  curl -fsSL "$REL/v$SHERPA_VERSION/sherpa-onnx-v$SHERPA_VERSION-osx-arm64-shared-no-tts-lib.tar.bz2" | tar xj -C "$tmp"
  rm -rf "$DEST/sherpa-onnx/lib" && mv "$tmp"/*/lib "$DEST/sherpa-onnx/lib" && rm -rf "$tmp"
fi
if [[ ! -f "$DEST/sensevoice/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/model.int8.onnx" ]]; then
  echo "downloading SenseVoice-Small (int8, 2024-07-17)"
  curl -fsSL "$REL/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2" | tar xj -C "$DEST/sensevoice"
fi
if [[ ! -f "$DEST/sensevoice/silero_vad.onnx" ]]; then
  echo "downloading Silero VAD"
  curl -fsSL -o "$DEST/sensevoice/silero_vad.onnx" "$REL/asr-models/silero_vad.onnx"
fi
du -sh "$DEST"
