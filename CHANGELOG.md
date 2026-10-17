# Changelog

Changes in the Persian fork (hadi-buddy/foresight-zh), on top of
michaelpersonal/foresight-zh @ 8f15aea.

## 2026-10-17 — Remove the Chinese README

- **Context:** this fork only targets Persian.
- **Change:** deleted `README_zh.md` and its language links in `README.md` and `README_fa.md`.
  The Chinese code paths (SenseVoice engine, CJK spacing) stay as a fallback.
- **Status:** done.

## 2026-10-17 — Persian as the default engine

- **Context:** the fork should transcribe Persian without any extra configuration.
- **Change:** `load_config` in `shim/foresight_zh.c` defaults to `persian` when
  `~/tools/foresight-zh/engine` is absent. `setup-persian.sh` removes that file instead of
  writing it. Fallback order: Persian → SenseVoice → Foresight's own English engine.
- **Status:** done; not yet run on a Mac.

## 2026-10-17 — Persian transcription engine

- **Context:** SenseVoice-Small only covers zh/en/yue/ja/ko, so it cannot transcribe Persian.
- **Change:**
  - `shim/sensevoice_engine.c`: `sv_available(home, lang)` selects SenseVoice or
    Shenava Rizeh v1.0 (Persian NeMo CTC, Apache-2.0) via `model_config.nemo_ctc`.
    The model's ONNX metadata disables NeMo feature normalization, so the feature config
    is left untouched.
  - `shim/persian_itn.c` (new, written from scratch): spoken Persian numbers → Persian
    digits; a lone «یک» stays a word. Tests: `shim/test/itn_test.sh` (17 cases, passing).
  - `shim/chat_bridge.c`: Arabic-script share ≥ 30% → ask the chat model for Persian output.
  - `setup-persian.sh`, `README_fa.md`, Persian section in `README.md`.
- **Status:** syntax-checked and unit-tested on Linux; full build and live transcription
  still to be verified on macOS (look for `asr (persian): ready` in `shim.log`).
