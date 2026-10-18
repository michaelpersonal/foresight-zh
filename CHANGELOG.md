# Changelog

Farsight (hadi-buddy/farsight), built on michaelpersonal/foresight-zh @ 8f15aea.

## 2026-10-18 — Lower Gemma temperature for Persian summaries

- **Context:** Gemma's Persian summaries had garbled words in every run («تحلی» for «تحلیلی»,
  «تعاری» for «تعاریف», «موفقیت‌آمی», «مثلهفعل»). Foresight samples Gemma at temperature 0.4 /
  top_k 40, which a small on-device model handles poorly in Persian.
- **Change:** `gemma_create()` in `shim/chat_bridge.c` wraps `litert_lm_create_conversation_v2`
  for conversations served by Gemma and sets every `temperature` / `top_k` in the config to
  0.1 / 10. `FARSIGHT_GEMMA_SAMPLING="<temp>,<top_k>"` changes the values, `app` keeps Foresight's.
  Logs `chat: Gemma sampling …` once.
- **Status:** config rewrite unit-tested; to verify on macOS that summaries have fewer misspellings.

## 2026-10-18 — Persian summary headings

- **Context:** Persian summaries still had the English template headings «Overview» and «Details»,
  which the language instruction asked the model to keep.
- **Change:** `persian_note()` in `shim/chat_bridge.c`: for a Persian transcript the model is asked to
  write the headings in Persian too («# مرور کلی», «# جزئیات»); JSON keys stay in English.
  `FARSIGHT_HEADINGS=en ./launch.sh` restores the English headings. Applies to Gemma and the
  local-server bridge.
- **Status:** to verify on macOS that Foresight still shows the summary correctly (the app might
  use the English headings to split sections).
- **Not changed:** UI labels hard-coded in Foresight («Start transcribing», «Live Q&A», «Now»,
  «Transcript», «Start new session», «Link scheduled meeting») are compiled into Google's binary and
  absent from its Persian translation table; Farsight does not modify the app, so they stay English.

## 2026-10-18 — Echo dedup, Persian summaries from Gemma, «نه» stays a word

- **Context:** a real test (a video playing through the speakers) gave every sentence twice,
  once as "Microphone" and once as "Remote" (system audio); phrases cut by the 15 s limit
  ended in a wrong «.»; «نه» ("not") became «۹»; Foresight's summary came back in English.
- **Change:**
  - `shim/sensevoice_engine.c`: cross-stream dedup. Each phrase is compared, by word pairs
    (single words like «و» match everything), with what the other stream said in the last
    30 s. If ≥60 % of its pairs are there it is a repeat: dropped, or trimmed to its new
    trailing words (3+). Repeats are still remembered for the other stream. On the test
    transcript: 33 lines → 20, no content lost. Tuning: `dedup`, `dedup_window`, `dedup_overlap`.
  - A phrase cut by `max_speech` (now 20 s) gets no final «.» (`persian_punctuate(text, complete)`).
  - `shim/persian_itn.c`: a lone «نه» is left as a word, like «یک»; «بیست و نه», «نه هزار» still convert.
  - `shim/chat_bridge.c`: when Gemma serves a request (bridge off or server down), a summary
    request with a Persian transcript gets the "write in Persian" instruction appended to the
    user message. The Persian share is now measured inside the transcript tags only.
- **Status:** dedup tested on the real transcript, other parts unit-tested; to verify on macOS.

## 2026-10-18 — Punctuation

- **Context:** with Koochik and ZWNJ the live test was accurate («در جلسه دیزاین امروز ما بررسی
  می‌کنیم که فیدبک‌ها همیشه بهش پرداخته بشه»), but Shenava outputs no punctuation.
- **Change:** `persian_punctuate` in `shim/persian_text.c`, applied per phrase (a phrase ends at a
  ~0.9 s pause): «؟» after a leading question word (چرا، چطور، کی، …) or a tag («… نه»); no mark
  when the phrase ends on a connector (که، و، ولی، به، …) since the sentence continues; «.»
  otherwise; «،» before اما/ولی/چون/یعنی after two or more words. `punctuation = 0` in the
  tuning file turns it off. Tests in `shim/test/text_test.c`.
- **Status:** unit tests pass; to verify on macOS.

## 2026-10-18 — Tuning file, postroll, phrase audio dump

- **Context:** recognition still makes mistakes with Koochik; tuning needs real examples and
  parameters that can change without a rebuild.
- **Change:** `~/tools/farsight/tuning` (see `tuning.example`) sets vad_threshold, min_silence,
  min_speech, max_speech, preroll, postroll, threads, dump; values are logged as `tuning:`.
  Phrases are now decoded with postroll (0.15 s) as well as preroll. `dump = 1` (or
  `FARSIGHT_DUMP=1`) writes each phrase as 16 kHz WAV plus `index.tsv` in
  `~/Library/Logs/farsight/audio/`.
- **Status:** compiles; to verify on macOS. Vazirmatn verified on macOS (whole UI in Vazirmatn).

## 2026-10-18 — Vazirmatn font for Persian text

- **Context:** Persian text in Foresight used the macOS fallback font; Vazirmatn is the expected
  Persian UI font.
- **Change:**
  - `shim/persian_font.c` (new): registers `~/Library/Application Support/farsight/fonts/Vazirmatn-*.ttf`
    for the process and interposes `CTFontCreateForString` / `…WithLanguage`. For a run that starts
    with an Arabic-script character it returns Vazirmatn at the same size and the nearest of nine
    weights (from the requested font's `kCTFontWeightTrait`); everything else is unchanged.
    Logs `font: Vazirmatn in use` the first time. `FARSIGHT_FONT=system` disables it.
  - `setup-persian.sh` downloads the nine static Vazirmatn TTFs (pinned commit 6e553e3, SIL OFL 1.1).
  - `shim/build.sh` links CoreText and CoreFoundation.
- **Status:** verified on macOS 2026-10-18.

## 2026-10-18 — Bigger Persian model; longer phrase pause

- **Context:** with Rizeh (32M) the live test gave «حدمند» for «هدفمند», and the phrase was split
  mid-word into «حدمند» + «ذ بشه» at a short pause.
- **Change:**
  - `setup-persian.sh` downloads Shenava Koochik v1.0 (114M, same NeMo CTC API; 7.49% WER /
    2.30% CER on visualears-golden-6669) by default; `FARSIGHT_MODEL=rizeh` keeps the small one.
  - The engine prefers `persian/shenava-koochik-v1.0`, falling back to Rizeh, and logs which.
  - VAD `min_silence_duration` 0.6 s → 0.9 s for Persian, so short pauses don't split phrases.
- **Status:** to verify on macOS.

## 2026-10-18 — Persian UI by default

- **Context:** testing `FARSIGHT_LANG=fa` showed Foresight ships a Persian translation: the whole UI
  switched to Persian and right-to-left, with no patching needed.
- **Change:** `launch.sh` now starts Foresight in Persian by default (`-AppleLanguages (fa)`,
  `-AppleLocale fa_IR`); `FARSIGHT_LANG=en` or `system` overrides. READMEs updated; the
  "left-to-right layout" limitation is gone.
- **Status:** verified on macOS (Persian RTL UI).

## 2026-10-18 — Persian orthography, VAD pre-roll, RTL experiment

- **Context:** the first live run showed «می شه» / «طرح های» without ZWNJ, and a clipped first
  syllable («ظه ضبط»).
- **Change:**
  - `shim/persian_text.c` (new): ي/ى→ی, ك→ک, Arabic-Indic→Persian digits; ZWNJ for «می/نمی + verb»,
    «word + ها/های/…», «…ه + ای». Whole tokens only. Tests in `shim/test/text_test.c`.
  - `shim/sensevoice_engine.c`: per-stream 32 s ring of 16 kHz audio; each phrase is decoded with
    0.35 s of audio before the VAD start. Output goes through ITN, then `persian_fix`.
  - `launch.sh`: `FARSIGHT_LANG=fa` starts Foresight with that UI language, to test whether the
    app lays out right-to-left.
- **Status:** unit tests pass on Linux; to verify on macOS.

## 2026-10-18 — First live run on macOS

- **Context:** first end-to-end test on the maintainer's Mac (Apple Silicon, Foresight with Gemma 4 E4B).
- **Result:** `config: engine=persian`, `filter patch: applied`, `asr (persian): ready`; Persian speech
  shows up in Foresight's Transcript tab phrase by phrase, letters joined correctly.
- **Seen in the test:** the first syllable of a phrase can be clipped by VAD («ظه ضبط» for «...ه ضبط»);
  no zero-width non-joiner in «می شه», «طرح های».
- **Status:** working. Next: ZWNJ/Arabic-letter normalization, VAD pre-roll.

## 2026-10-18 — Resumable downloads in setup-persian.sh

- **Context:** on a slow or unstable connection the sherpa-onnx download (silent `curl | tar`)
  looked frozen, and any drop meant starting over.
- **Change:** a `fetch` helper downloads to `<file>.part` with a progress bar and resumes with
  `curl -C -` after a drop (up to 50 tries; a stall under 1 KB/s for 60 s counts as a drop).
  Used for sherpa-onnx, Shenava and Silero VAD.
- **Status:** done.

## 2026-10-17 — Rename to Farsight; new README

- **Context:** the fork should stand as its own project, findable by people searching for
  Persian speech-to-text on macOS, instead of reading as a copy of foresight-zh.
- **Change:**
  - Paths: repo `~/tools/foresight-zh` → `~/tools/farsight`; logs `~/Library/Logs/farsight`;
    models `~/Library/Application Support/farsight` (was `~/local-llm/models/special/foresight-zh`).
  - `shim/foresight_zh.c` → `shim/farsight.c`, output `libfarsight.dylib`; env vars
    `FORESIGHT_ZH_*` → `FARSIGHT_*`; LaunchAgent label `io.github.hadi-buddy.farsight.watcher`.
  - `launch.sh` finds the shim next to itself instead of a hard-coded path.
  - `README.md` and `README_fa.md` rewritten from scratch: Persian-first, install steps,
    limitations, credits to foresight-zh and Shenava.
- **Status:** done; GitHub repo renamed to `farsight`.

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
- **Status:** done; verified on macOS 2026-10-18.

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
- **Status:** verified on macOS 2026-10-18 (see "First live run").
