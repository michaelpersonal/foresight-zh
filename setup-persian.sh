#!/bin/zsh
# Downloads the Persian engine: sherpa-onnx C library, Shenava Rizeh v1.0 (Persian
# NeMo CTC, Apache-2.0) and Silero VAD. Persian is the default engine.
# Installs into ~/local-llm/models/special/foresight-zh (about 150MB).
set -e
SHERPA_VERSION=1.13.8
DEST=${FORESIGHT_ZH_MODELS:-$HOME/local-llm/models/special/foresight-zh}
REL=https://github.com/k2-fsa/sherpa-onnx/releases/download
HF=https://huggingface.co/Reza2kn/Shenava-Rizeh-v1.0-sherpa-onnx/resolve/main
FA="$DEST/persian/shenava-rizeh-v1.0"
mkdir -p "$DEST/sensevoice" "$DEST/sherpa-onnx" "$FA"

if [[ ! -f "$DEST/sherpa-onnx/lib/libsherpa-onnx-c-api.dylib" ]]; then
  echo "downloading sherpa-onnx $SHERPA_VERSION"
  tmp=$(mktemp -d)
  curl -fsSL "$REL/v$SHERPA_VERSION/sherpa-onnx-v$SHERPA_VERSION-osx-arm64-shared-no-tts-lib.tar.bz2" | tar xj -C "$tmp"
  rm -rf "$DEST/sherpa-onnx/lib" && mv "$tmp"/*/lib "$DEST/sherpa-onnx/lib" && rm -rf "$tmp"
fi
for f in model.onnx tokens.txt LICENSE; do
  if [[ ! -s "$FA/$f" ]]; then
    echo "downloading Shenava Rizeh v1.0: $f"
    curl -fL --progress-bar -o "$FA/$f.part" "$HF/$f" && mv "$FA/$f.part" "$FA/$f" ||
      { [[ $f == LICENSE ]] && rm -f "$FA/$f.part" || exit 1; }
  fi
done
if [[ ! -f "$DEST/sensevoice/silero_vad.onnx" ]]; then
  echo "downloading Silero VAD"
  curl -fsSL -o "$DEST/sensevoice/silero_vad.onnx" "$REL/asr-models/silero_vad.onnx"
fi
rm -f "${0:A:h}/engine"  # no override: the shim defaults to persian
du -sh "$DEST"
