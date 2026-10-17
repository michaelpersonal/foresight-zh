# foresight-zh

**English** · [فارسی](README_fa.md)

Chinese (and mixed Chinese/English) or Persian transcription for Google AI Edge Foresight on
macOS, without modifying the app. Optionally, Foresight's chat runs on a bigger local model
(Qwen3.8-27B) instead of the built-in Gemma 4 E2B.

Unofficial and not affiliated with Google. No Google code or model weights are included.

## Use

- Launch **`~/Applications/Foresight 中文.app`** (or run `~/tools/foresight-zh/launch.sh`).
  It quits a running Foresight and relaunches it with the shim.
- Launching Foresight any other way (Dock, Spotlight, login item, a notification) starts it
  *without* the shim: it still works, but transcribes English only. The optional watcher
  below fixes that automatically.
- Text appears once per spoken phrase, after each pause, rather than word by word.
- Log: `~/Library/Logs/foresight-zh/shim.log`. Debug run that also logs every transcript
  result: `FORESIGHT_ZH_TRACE=1 ~/tools/foresight-zh/launch.sh`.

- Chat goes to the local Qwen server when it is running and to Foresight's own Gemma
  when it is not (see "Chat on a local model" below).

## Setup

The shim and launcher expect this repo at `~/tools/foresight-zh`.

1. Install Foresight and open it once.
2. Download the transcription engine, about 270MB: the
   [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) C library,
   SenseVoice-Small (int8) and Silero VAD. They go to `~/local-llm/models/special/foresight-zh/`.

   ```bash
   ./setup-sensevoice.sh
   ```

   For Persian instead, run `./setup-persian.sh` (about 150MB). It downloads
   [Shenava Rizeh v1.0](https://huggingface.co/Reza2kn/Shenava-Rizeh-v1.0-sherpa-onnx)
   (Persian FastConformer CTC, Apache-2.0). In this fork Persian is the default engine;
   `echo sensevoice > ~/tools/foresight-zh/engine` switches back. See "Persian" below.

3. Build the shim:

   ```bash
   shim/build.sh
   ```

4. Optional, make a Dock/Spotlight launcher:

   ```bash
   osacompile -o ~/Applications/"Foresight 中文.app" -e 'do shell script "$HOME/tools/foresight-zh/launch.sh"'
   ```

5. Optional, install the auto-relaunch watcher. This LaunchAgent checks every 5 seconds,
   and if Foresight is running without the shim, relaunches it through `launch.sh`. That
   covers plain launches from the Dock, notifications, login items and updates. The restart
   happens a few seconds after a plain launch, so avoid starting a recording in that
   window. It relaunches at most 3 times in 5 minutes.

   ```bash
   ./install-watcher.sh              # install and start
   ./install-watcher.sh --uninstall  # remove
   touch ~/Library/Logs/foresight-zh/watcher.paused   # pause (rm the file to resume)
   ```

   Log: `~/Library/Logs/foresight-zh/watcher.log`.

## How it works

Foresight's live transcription runs through Google's closed ODML transcription library. The
Dart UI calls it through an exported C API (`OdmlCreateInferenceEngine`,
`OdmlCreateSpeechPipelineFromProto`, `OdmlSpeechProcessorTranscribe`,
`OdmlSpeechProcessorFinishStream`, ...). Its model (`models/3`) is English-only.

`shim/libforesight_zh.dylib` is injected with `DYLD_INSERT_LIBRARIES`. The app's
entitlements allow that (`allow-dyld-environment-variables`, `disable-library-validation`),
so Google's code signature and keychain access are untouched. The shim does three things for transcription:

1. **Replaces speech recognition.** It interposes `dlsym`, which Dart FFI uses to find the C
   API, and wraps `OdmlSpeechProcessorTranscribe` and `OdmlSpeechProcessorFinishStream`.
   The audio Foresight passes in (48kHz float, one stream for the mic and one for system
   audio) is resampled to 16kHz and split into phrases with Silero VAD. Each phrase is
   transcribed with SenseVoice-Small on the CPU, typically in 15–85ms. The text goes back in
   Foresight's own result format (`text`, `is_final`, start/end in microseconds).
   Foresight's English engine still loads but never runs. See `shim/sensevoice_engine.c`.
2. **Removes the ASCII filter.** The Dart layer strips everything outside printable ASCII
   from transcripts (`[^\x20-\x7E\n\r]`), which deletes all Chinese. The shim patches that
   regex constant in memory (copy-on-write page; the binary on disk is unchanged) to
   `[\x00-\x08\x7F]+`.

3. **Spaces Chinese for Foresight's word counts.** Foresight measures transcripts in
   space-separated words. It builds the live transcript chunks that chat searches, and offers
   the meeting summary, only past a word threshold, so unspaced Chinese looked nearly empty:
   no summary button and no meeting context in chat. The shim puts a hair space (U+200A,
   near-invisible) at every boundary next to a CJK character. The chat bridge removes them
   again before text reaches the model.

If the SenseVoice files are missing or fail to load, the shim falls back to Foresight's own
engine (English).

### Persian

The default engine in this fork (`persian`, or `fa`) makes the shim transcribe each phrase
with Shenava Rizeh instead of SenseVoice, through the same sherpa-onnx library, VAD and
result path. Differences from the Chinese engine:

- The model is Persian-only. English words inside Persian speech come out as Persian-script
  approximations, and English-only audio is not transcribed well. Switch the engine back for
  English or Chinese meetings.
- Shenava spells numbers out. `shim/persian_itn.c` rewrites well-formed spoken numbers as
  Persian digits (`بیست و پنج` → `۲۵`, `هزار و چهارصد و سه` → `۱۴۰۳`). A lone `یک` stays a
  word, since it is usually the article "a". Tests: `shim/test/itn_test.sh`.
- The model reads un-normalized log-mel features; its ONNX metadata tells sherpa-onnx to skip
  NeMo's per-feature normalization. Do not override the feature config.
- Persian is space-separated, so the CJK hair-space step does nothing to it.
- Summaries through the chat bridge: for a mostly Arabic-script transcript the bridge asks for
  Persian output, keeping the template headings.
- Foresight's UI lays text out left-to-right. Letters join correctly, but lines stay
  left-aligned and mixed Persian/English runs can look out of order.

If the Persian model is missing, the shim logs it and falls back to SenseVoice, then to
Foresight's own English engine.

### Alternative engine: Gemma 4 E2B

Before SenseVoice, the shim ran Foresight's own pipeline on a repack of the Gemma 4 E2B
model that Foresight downloads. That approach redirects the model path and rewrites the
pipeline options for batch decoding with a transcription prompt. It works, but needs a 3.4GB
copy of Gemma and more memory. To use it:

```bash
uv venv ~/local-llm/venvs/litert --python 3.12
uv pip install -p ~/local-llm/venvs/litert/bin/python litert-lm-api litert-lm
model-build/build.sh                       # repacks models/15 into the ASR bundle
echo gemma > ~/tools/foresight-zh/engine   # switch engines; delete the file to go back
```

`model-build/build.sh` drops the vision and drafter sections and the CPU-only tag on the
audio sections, because Foresight runs speech on the GPU. It also swaps in a
transcription-only template. `make_override.py` generates `pipeline_override.pb`, the options
substituted in Gemma mode.

## Chat on a local model

Foresight's chat (questions about your notes and calendar) runs Gemma 4 E2B on-device. The
shim can send it to a local OpenAI-compatible server instead. The default target is the
Splash server for Qwen3.8-27B in `~/local-llm`:

```bash
~/local-llm/runtime/bin/splash-serve start qwen38-27b-splash
echo 'http://127.0.0.1:8000/v1 incoai/Qwen3.8-27B-Splash {"reasoning_effort":"none","temperature":0.7,"top_p":0.8,"top_k":20}' > ~/tools/foresight-zh/chat
```

The `chat` file is one line: `<base_url> [model] [{extra request fields}]`, or `off`. Without
the file the shim uses `http://127.0.0.1:8096/v1 qwen3.8-27b-mtp` (llama.cpp). Relaunch
Foresight after editing it.

- The shim never starts the server: in `~/local-llm` only one model server runs at a time,
  and starting one evicts the others. When the server is down, or a different model is on
  the port, chat uses Gemma. The check is repeated every 5 seconds.
- Foresight's own sampling (temperature 0.4, top_k 40, tuned for Gemma) is not forwarded.
  The llama.cpp profile applies Qwen's non-thinking settings itself. Splash has no server
  defaults, so the `chat` line above sends them: thinking off plus temperature 0.7, top_p
  0.8, top_k 20. Splash rejects `presence_penalty`, the fourth setting Qwen recommends.
- Replies are capped at 2048 tokens, because llama.cpp keeps generating after a cancel.
- Measured on Foresight queries (M5 Max): Qwen3.8-27B on Splash wrote ~212 chars/s,
  against ~91 on llama.cpp. Qwen3.8-Flash-Next (MTPLX) was as fast but ignored retrieved
  notes on one query and called the calendar tool instead.

- Meeting summaries are generated through the same bridge. Foresight's summary prompts are
  English, so for a mostly-Chinese transcript the bridge asks for Simplified Chinese output,
  keeping the template headings (`# Overview`, `# Details`).
- Limits: during a meeting, Foresight finds transcript passages by searching with your
  question, so ask in the meeting's language (English questions do not match Chinese
  passages). Meetings recorded before the spacing change keep unspaced transcripts and get
  no summary button.

How it works: Foresight's chat uses LiteRT-LM's conversation C API with JSON messages
(`litert_lm_create_conversation_v2`, `litert_lm_conversation_send_message_stream`, ...).
The shim wraps those through the same `dlsym` interposition, converts the conversation
(system prompt, notes context, history, the `search_calendar` tool) to an OpenAI chat
request, streams the reply back in Foresight's chunk format, and turns tool calls into
Foresight's tool-call messages. Other uses of the engine (summaries, audio, images) still
run on Gemma. `FORESIGHT_ZH_TRACE=1` logs every `dlsym` lookup and every other `litert_lm_*` call, and
saves their JSON arguments and the bridge's requests to `~/Library/Logs/foresight-zh/lm/`. `shim/test/chat_harness.c` replays
saved conversations through the bridge.

## When Foresight updates

The shim checks before acting and logs what it skipped:

- **Filter regex not found:** the patch is skipped and Chinese may get stripped again. Find
  the new filter string and update `patch_ascii_filter` in `shim/foresight_zh.c`.
- **Chat C API changed:** check `shim.log` for `chat:` lines, set `chat` to `off`, and
  re-trace with `FORESIGHT_ZH_TRACE=1`.
- **Speech C API changed:** the wrappers depend on the current argument layout and result
  struct. Check `shim.log`. Without the shim, the app is unaffected.

## Files

- `shim/foresight_zh.c`: dlsym interposition, ODML hooks, filter patch, engine selection
- `shim/sensevoice_engine.c`: VAD + SenseVoice or Shenava (Persian) engine via sherpa-onnx (loaded with `dlopen`)
- `shim/persian_itn.c`: spoken Persian numbers to digits; `shim/test/itn_test.sh` tests it
- `shim/chat_bridge.c`: chat on a local OpenAI-compatible server (libcurl)
- `shim/cJSON.c`, `shim/cJSON.h`: [cJSON](https://github.com/DaveGamble/cJSON) v1.7.18 (MIT)
- `shim/test/chat_harness.c`: replays saved Foresight conversations through the chat bridge
- `shim/sherpa-onnx-c-api.h`: sherpa-onnx C API header (v1.13.8, Apache-2.0, Xiaomi Corporation)
- `shim/test/sv_harness.c`: feeds a raw 48kHz float32 capture through the engine
- `setup-sensevoice.sh`, `setup-persian.sh`: download the engines; `shim/build.sh`: builds the shim
- `launch.sh`: launcher; `~/Applications/Foresight 中文.app` wraps it
- `watcher.sh`, `install-watcher.sh`: auto-relaunch LaunchAgent (detects the shim through
  `~/Library/Logs/foresight-zh/shim.pid`)
- `model-build/`, `make_override.py`, `pipeline_override.pb`: the Gemma alternative
