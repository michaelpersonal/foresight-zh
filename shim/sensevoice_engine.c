// SenseVoice transcription engine for the farsight shim.
//
// Replaces Foresight's ASR inference: the shim hands us the 48 kHz float audio
// Foresight passes to OdmlSpeechProcessorTranscribe, we resample it to 16 kHz,
// segment it with Silero VAD and transcribe each finished segment with
// SenseVoice (Chinese/English) or Shenava Rizeh (Persian, NeMo CTC), both via
// sherpa-onnx on the CPU. sherpa-onnx is dlopen'ed so the shim still
// loads (and the app still starts) if the library is missing.

#include "sensevoice_engine.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "persian_itn.h"
#include "persian_text.h"
#include "sherpa-onnx-c-api.h"

#define IN_RATE 48000
#define VAD_RATE 16000
#define MAX_STREAMS 4
// Silero VAD marks a phrase a little after speech begins, which clips the first
// syllable ("ظه ضبط" for "...ه ضبط"). Each stream keeps its recent 16 kHz audio
// in a ring so a phrase can be decoded with extra audio around it (preroll/postroll).
#define RING (VAD_RATE * 32)            // longer than max_speech_duration + silence

// Tuning, overridable from ~/tools/farsight/tuning (one "key = value" per line, # comments):
//   vad_threshold  0.5    speech probability that starts a phrase (lower = more sensitive)
//   min_silence    0.9    pause (s) that ends a phrase (Persian; 0.6 for other engines)
//   min_speech     0.25   shortest phrase (s)
//   max_speech     20     longest phrase (s) before a forced break
//   preroll        0.35   audio (s) added before each phrase
//   postroll       0.15   audio (s) added after each phrase
//   threads        2      CPU threads for the speech model
//   dump           0      1 = save each phrase as WAV + text in ~/Library/Logs/farsight/audio/
//   punctuation    1      0 = no «.» «؟» «،» added to phrases
//   dedup          1      drop a phrase that repeats what the other stream (mic vs. system
//                         audio) just said: speakers picked up by the microphone
//   dedup_window   30     how far back (s) to look in the other stream
//   dedup_overlap  0.6    share of a phrase's word pairs found there that counts as a repeat
static struct {
  float vad_threshold, min_silence, min_speech, max_speech, preroll, postroll;
  int threads, dump, punctuation, dedup;
  float dedup_window, dedup_overlap;
} T = {0.5f, -1, 0.25f, 20.0f, 0.35f, 0.15f, 2, 0, 1, 1, 30.0f, 0.6f};
static char g_dump_dir[1100];
static int g_dump_n;

extern void logf_(const char *fmt, ...);

#define API(name) static __typeof__(&name) p_##name
API(SherpaOnnxCreateOfflineRecognizer);
API(SherpaOnnxCreateOfflineStream);
API(SherpaOnnxDestroyOfflineStream);
API(SherpaOnnxAcceptWaveformOffline);
API(SherpaOnnxDecodeOfflineStream);
API(SherpaOnnxGetOfflineStreamResult);
API(SherpaOnnxDestroyOfflineRecognizerResult);
API(SherpaOnnxCreateVoiceActivityDetector);
API(SherpaOnnxVoiceActivityDetectorAcceptWaveform);
API(SherpaOnnxVoiceActivityDetectorEmpty);
API(SherpaOnnxVoiceActivityDetectorFront);
API(SherpaOnnxVoiceActivityDetectorPop);
API(SherpaOnnxDestroySpeechSegment);
API(SherpaOnnxVoiceActivityDetectorFlush);
API(SherpaOnnxVoiceActivityDetectorReset);
API(SherpaOnnxCreateLinearResampler);
API(SherpaOnnxLinearResamplerReset);
API(SherpaOnnxLinearResamplerResample);
API(SherpaOnnxLinearResamplerResampleFree);

typedef struct {
  const SherpaOnnxVoiceActivityDetector *vad;
  const SherpaOnnxLinearResampler *resampler;
  float *ring;    // last RING samples fed to the VAD
  long long fed;  // samples fed since the last reset
} Stream;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_state;  // 0 loading, 1 ready, -1 failed
static const SherpaOnnxOfflineRecognizer *g_rec;
static SherpaOnnxVadModelConfig g_vad_cfg;
static Stream g_streams[MAX_STREAMS];
static int g_lang;  // SV_LANG_*
static char g_dir[1024], g_model[1200], g_tokens[1200], g_vad_model[1200], g_lib[1200];

static int exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

static int load_symbols(void) {
  void *h = dlopen(g_lib, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    logf_("sensevoice: dlopen %s failed: %s", g_lib, dlerror());
    return 0;
  }
#define LOAD(name)                                                       \
  if (!(p_##name = (__typeof__(&name))dlsym(h, #name))) {               \
    logf_("sensevoice: missing symbol %s", #name);                       \
    return 0;                                                            \
  }
  LOAD(SherpaOnnxCreateOfflineRecognizer);
  LOAD(SherpaOnnxCreateOfflineStream);
  LOAD(SherpaOnnxDestroyOfflineStream);
  LOAD(SherpaOnnxAcceptWaveformOffline);
  LOAD(SherpaOnnxDecodeOfflineStream);
  LOAD(SherpaOnnxGetOfflineStreamResult);
  LOAD(SherpaOnnxDestroyOfflineRecognizerResult);
  LOAD(SherpaOnnxCreateVoiceActivityDetector);
  LOAD(SherpaOnnxVoiceActivityDetectorAcceptWaveform);
  LOAD(SherpaOnnxVoiceActivityDetectorEmpty);
  LOAD(SherpaOnnxVoiceActivityDetectorFront);
  LOAD(SherpaOnnxVoiceActivityDetectorPop);
  LOAD(SherpaOnnxDestroySpeechSegment);
  LOAD(SherpaOnnxVoiceActivityDetectorFlush);
  LOAD(SherpaOnnxVoiceActivityDetectorReset);
  LOAD(SherpaOnnxCreateLinearResampler);
  LOAD(SherpaOnnxLinearResamplerReset);
  LOAD(SherpaOnnxLinearResamplerResample);
  LOAD(SherpaOnnxLinearResamplerResampleFree);
  return 1;
}

static void *load_thread(void *arg) {
  (void)arg;
  int ok = load_symbols();
  if (ok) {
    SherpaOnnxOfflineRecognizerConfig c;
    memset(&c, 0, sizeof c);
    c.feat_config.sample_rate = VAD_RATE;
    c.feat_config.feature_dim = 80;
    if (g_lang == SV_LANG_PERSIAN) {
      // Shenava's ONNX metadata disables NeMo feature normalization; leave the
      // feature config alone so sherpa-onnx honors it.
      c.model_config.nemo_ctc.model = g_model;
    } else {
      c.model_config.sense_voice.model = g_model;
      c.model_config.sense_voice.language = "auto";
      c.model_config.sense_voice.use_itn = 1;
    }
    c.model_config.tokens = g_tokens;
    c.model_config.num_threads = T.threads;
    c.model_config.provider = "cpu";
    c.decoding_method = "greedy_search";
    g_rec = p_SherpaOnnxCreateOfflineRecognizer(&c);
    if (!g_rec) logf_("sensevoice: recognizer creation failed");
    ok = g_rec != NULL;
  }
  if (ok) {
    memset(&g_vad_cfg, 0, sizeof g_vad_cfg);
    g_vad_cfg.silero_vad.model = g_vad_model;
    g_vad_cfg.silero_vad.threshold = T.vad_threshold;
    // Pause that ends a phrase. Persian speakers often pause briefly inside a
    // phrase; 0.6 s split words in two («هدفمند» -> «حدمند» + «ذ بشه»).
    g_vad_cfg.silero_vad.min_silence_duration =
        T.min_silence > 0 ? T.min_silence : (g_lang == SV_LANG_PERSIAN ? 0.9f : 0.6f);
    g_vad_cfg.silero_vad.min_speech_duration = T.min_speech;
    g_vad_cfg.silero_vad.max_speech_duration = T.max_speech;
    g_vad_cfg.silero_vad.window_size = 512;
    g_vad_cfg.sample_rate = VAD_RATE;
    g_vad_cfg.num_threads = 1;
    g_vad_cfg.provider = "cpu";
  }
  pthread_mutex_lock(&g_mu);
  g_state = ok ? 1 : -1;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);
  logf_("tuning: vad_threshold=%.2f min_silence=%.2f min_speech=%.2f max_speech=%.1f preroll=%.2f "
        "postroll=%.2f threads=%d dump=%d punctuation=%d dedup=%d/%.0fs/%.2f",
        T.vad_threshold, g_vad_cfg.silero_vad.min_silence_duration, T.min_speech, T.max_speech, T.preroll,
        T.postroll, T.threads, T.dump, T.punctuation, T.dedup, T.dedup_window, T.dedup_overlap);
  logf_("asr (%s): %s", g_lang == SV_LANG_PERSIAN ? "persian" : "sensevoice", ok ? "ready" : "unavailable");
  return NULL;
}

static void load_tuning(const char *home) {
  char p[1100], line[256];
  snprintf(p, sizeof p, "%s/tools/farsight/tuning", home);
  FILE *f = fopen(p, "r");
  if (f) {
    while (fgets(line, sizeof line, f)) {
      char key[64];
      float v;
      if (line[0] == '#' || sscanf(line, " %63[a-z_] = %f", key, &v) != 2) continue;
      if (!strcmp(key, "vad_threshold")) T.vad_threshold = v;
      else if (!strcmp(key, "min_silence")) T.min_silence = v;
      else if (!strcmp(key, "min_speech")) T.min_speech = v;
      else if (!strcmp(key, "max_speech")) T.max_speech = v > 28 ? 28 : v;  // stays inside the ring
      else if (!strcmp(key, "preroll")) T.preroll = v < 0 ? 0 : v > 1.5f ? 1.5f : v;
      else if (!strcmp(key, "postroll")) T.postroll = v < 0 ? 0 : v > 1.0f ? 1.0f : v;
      else if (!strcmp(key, "threads")) T.threads = v < 1 ? 1 : v > 8 ? 8 : (int)v;
      else if (!strcmp(key, "dump")) T.dump = v != 0;
      else if (!strcmp(key, "punctuation")) T.punctuation = v != 0;
      else if (!strcmp(key, "dedup")) T.dedup = v != 0;
      else if (!strcmp(key, "dedup_window")) T.dedup_window = v < 1 ? 1 : v > 120 ? 120 : v;
      else if (!strcmp(key, "dedup_overlap")) T.dedup_overlap = v < 0.2f ? 0.2f : v > 1 ? 1 : v;
      else logf_("tuning: unknown key %s", key);
    }
    fclose(f);
  }
  const char *d = getenv("FARSIGHT_DUMP");
  if (d && *d == '1') T.dump = 1;
  if (T.dump) {
    snprintf(g_dump_dir, sizeof g_dump_dir, "%s/Library/Logs/farsight/audio", home);
    mkdir(g_dump_dir, 0755);
  }
}

// Saves one decoded phrase as 16 kHz mono 16-bit WAV, plus a line in index.tsv.
static void dump_phrase(const float *x, int n, const char *text) {
  char p[1300];
  int id = ++g_dump_n;
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  char stamp[32];
  strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
  snprintf(p, sizeof p, "%s/%s-%03d.wav", g_dump_dir, stamp, id);
  FILE *f = fopen(p, "wb");
  if (!f) return;
  unsigned data = (unsigned)n * 2, riff = 36 + data, rate = VAD_RATE, byterate = VAD_RATE * 2, fmtlen = 16;
  unsigned short pcm = 1, ch = 1, align = 2, bits = 16;
  fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmtlen, 4, 1, f);
  fwrite(&pcm, 2, 1, f), fwrite(&ch, 2, 1, f), fwrite(&rate, 4, 1, f), fwrite(&byterate, 4, 1, f);
  fwrite(&align, 2, 1, f), fwrite(&bits, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f);
  for (int i = 0; i < n; i++) {
    float v = x[i] < -1 ? -1 : x[i] > 1 ? 1 : x[i];
    short sv = (short)(v * 32767);
    fwrite(&sv, 2, 1, f);
  }
  fclose(f);
  snprintf(p, sizeof p, "%s/index.tsv", g_dump_dir);
  f = fopen(p, "a");
  if (f) {
    fprintf(f, "%s-%03d.wav\t%.2f\t%s\n", stamp, id, n / (double)VAD_RATE, text ? text : "");
    fclose(f);
  }
}

int sv_available(const char *home, int lang) {
  load_tuning(home);
  g_lang = lang;
  snprintf(g_dir, sizeof g_dir, "%s/Library/Application Support/farsight", home);
  if (lang == SV_LANG_PERSIAN) {
    // Prefer Shenava Koochik (114M, more accurate); fall back to Rizeh (32M).
    static const char *models[] = {"shenava-koochik-v1.0", "shenava-rizeh-v1.0"};
    for (int i = 0; i < 2; i++) {
      snprintf(g_model, sizeof g_model, "%s/persian/%s/model.onnx", g_dir, models[i]);
      snprintf(g_tokens, sizeof g_tokens, "%s/persian/%s/tokens.txt", g_dir, models[i]);
      if (exists(g_model) && exists(g_tokens)) {
        logf_("asr: persian model %s", models[i]);
        break;
      }
    }
  } else {
    snprintf(g_model, sizeof g_model, "%s/sensevoice/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/model.int8.onnx", g_dir);
    snprintf(g_tokens, sizeof g_tokens, "%s/sensevoice/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/tokens.txt", g_dir);
  }
  snprintf(g_vad_model, sizeof g_vad_model, "%s/sensevoice/silero_vad.onnx", g_dir);
  snprintf(g_lib, sizeof g_lib, "%s/sherpa-onnx/lib/libsherpa-onnx-c-api.dylib", g_dir);
  const char *need[] = {g_model, g_tokens, g_vad_model, g_lib};
  for (int i = 0; i < 4; i++) {
    if (!exists(need[i])) {
      logf_("asr: missing %s", need[i]);
      return 0;
    }
  }
  return 1;
}

void sv_start_loading(void) {
  pthread_t t;
  pthread_create(&t, NULL, load_thread, NULL);
  pthread_detach(t);
}

static int wait_ready(void) {
  pthread_mutex_lock(&g_mu);
  while (g_state == 0) pthread_cond_wait(&g_cv, &g_mu);
  int s = g_state;
  pthread_mutex_unlock(&g_mu);
  return s == 1;
}

static Stream *get_stream(int id) {
  Stream *s = &g_streams[id & (MAX_STREAMS - 1)];
  if (!s->vad) {
    s->vad = p_SherpaOnnxCreateVoiceActivityDetector(&g_vad_cfg, 60.0f);
    s->resampler = p_SherpaOnnxCreateLinearResampler(IN_RATE, VAD_RATE, VAD_RATE * 0.45f, 6);
    s->ring = calloc(RING, sizeof(float));
    s->fed = 0;
  }
  return s;
}

static void ring_push(Stream *s, const float *x, int n) {
  if (!s->ring) return;
  for (int i = 0; i < n; i++) s->ring[(s->fed + i) % RING] = x[i];
  s->fed += n;
}

// Persian post-processing: spoken numbers -> digits, orthography (ZWNJ, ی/ک),
// then punctuation (unless "punctuation = 0" in the tuning file).
static char *postprocess(const char *raw, int complete) {
  if (g_lang != SV_LANG_PERSIAN) return strdup(raw);
  char *a = persian_itn(raw);
  char *b = persian_fix(a);
  free(a);
  if (!T.punctuation) return b;
  char *c = persian_punctuate(b, complete);
  free(b);
  return c;
}

// ---- duplicate phrases across streams --------------------------------------
// Foresight records the microphone and the system audio as two streams. Without
// headphones the microphone also hears the speakers, so the same sentence comes
// back twice, slightly differently. A phrase whose word pairs are mostly in what
// the other stream said in the last dedup_window seconds is dropped.

#define RECENT 64
static struct {
  int stream;
  double t;
  char *text;
} g_recent[RECENT];
static int g_recent_n;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Normalizes one word: drops punctuation and ZWNJ so «می‌شه،» == «میشه».
static void norm_word(const char *p, size_t len, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < len;) {
    const unsigned char *u = (const unsigned char *)p + i;
    if (i + 2 < len + 0 && u[0] == 0xE2 && u[1] == 0x80 && u[2] == 0x8C) { i += 3; continue; }  // ZWNJ
    if (i + 1 < len && u[0] == 0xD8 && (u[1] == 0x8C || u[1] == 0x9F || u[1] == 0x9B)) { i += 2; continue; }
    if (strchr(".,?!:;", p[i])) { i++; continue; }
    if (o + 1 < cap) out[o++] = p[i];
    i++;
  }
  out[o] = 0;
}

typedef struct {
  const char *s;  // original token (with punctuation)
  size_t len;
  char w[96];     // normalized
} Word;

static int split_words(const char *text, Word *out, int max) {
  int n = 0;
  for (const char *p = text; *p && n < max;) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char *q = p;
    while (*q && *q != ' ') q++;
    out[n].s = p, out[n].len = (size_t)(q - p);
    norm_word(p, out[n].len, out[n].w, sizeof out[n].w);
    if (out[n].w[0]) n++;
    p = q;
  }
  return n;
}

// Checks a phrase against what the other stream said recently. Returns NULL to
// keep the phrase as is, "" to drop it, or the new part of a partly repeated
// phrase (malloc'ed): its trailing words that the other stream did not say.
static char *dedup(int stream, const char *text) {
  if (!T.dedup || !text) return NULL;
  static Word w[160], other[1200];
  int nw = split_words(text, w, 160);
  if (nw == 0) return NULL;
  double t = now_s();
  double window = nw < 3 ? 8.0 : T.dedup_window;  // short phrases: only very recent echoes
  int no = 0;
  for (int i = 0; i < g_recent_n && no < 1200; i++) {
    if (g_recent[i].stream == stream || t - g_recent[i].t > window) continue;
    no += split_words(g_recent[i].text, other + no, 1200 - no);
  }
  if (no == 0) return NULL;
  if (nw == 1) {
    for (int j = 0; j < no; j++)
      if (!strcmp(w[0].w, other[j].w)) return strdup("");
    return NULL;
  }
  // Word pairs, not single words: common words («و», «به», «که») would otherwise
  // make unrelated phrases look alike. A word in a matched pair is "covered".
  static unsigned char covered[160];
  memset(covered, 0, sizeof covered);
  int pairs = nw - 1, hit = 0;
  for (int i = 0; i < pairs; i++)
    for (int j = 0; j + 1 < no; j++)
      if (!strcmp(w[i].w, other[j].w) && !strcmp(w[i + 1].w, other[j + 1].w)) {
        covered[i] = covered[i + 1] = 1;
        hit++;
        break;
      }
  if (hit == 0) return NULL;
  double share = (double)hit / pairs;
  if (nw < 3) return share >= 0.999 ? strdup("") : NULL;
  if (share < T.dedup_overlap) return NULL;
  // Mostly a repeat. The other stream usually just hasn't reached the end yet,
  // so new content is at the end: keep the trailing words after the last
  // repeated stretch (a lone stray pair inside them is tolerated) if 3 or more.
  int from = nw, i = nw - 1;
  while (i >= 0) {
    if (!covered[i]) {
      from = i--;
      continue;
    }
    // A single stray pair («آقای دکتر») between new words is noise, not a repeat.
    int run = 0;
    while (i - run >= 0 && covered[i - run]) run++;
    if (run <= 2 && i - run - 1 >= 0 && !covered[i - run] && !covered[i - run - 1]) {
      i -= run;
      continue;
    }
    break;
  }
  size_t cap = strlen(text) + 8, o = 0;
  char *out = malloc(cap);
  if (nw - from >= 3) {
    for (int k = from; k < nw; k++) {
      if (o) out[o++] = ' ';
      memcpy(out + o, w[k].s, w[k].len);
      o += w[k].len;
    }
  }
  out[o] = 0;
  return out;
}

static void remember(int stream, const char *text) {
  if (g_recent_n == RECENT) {
    free(g_recent[0].text);
    memmove(g_recent, g_recent + 1, sizeof g_recent[0] * (RECENT - 1));
    g_recent_n--;
  }
  g_recent[g_recent_n].stream = stream;
  g_recent[g_recent_n].t = now_s();
  g_recent[g_recent_n].text = strdup(text);
  g_recent_n++;
}

// Transcribes every finished VAD segment; returns the number of results written.
static int drain(int stream, Stream *s, SvResult *out, int max_out) {
  int n = 0;
  while (!p_SherpaOnnxVoiceActivityDetectorEmpty(s->vad)) {
    const SherpaOnnxSpeechSegment *seg = p_SherpaOnnxVoiceActivityDetectorFront(s->vad);
    if (n < max_out && seg->n > 0) {
      // Decode the phrase with some audio before and after it, taken from the ring
      // (the VAD starts a phrase late and can end it on a trailing consonant).
      long long seg_end = seg->start + seg->n;
      long long from = seg->start - (long long)(T.preroll * VAD_RATE);
      long long to = seg_end + (long long)(T.postroll * VAD_RATE);
      if (from < 0) from = 0;
      if (to > s->fed) to = s->fed;
      if (s->fed - from > RING) from = s->fed - RING;
      const float *audio = seg->samples;
      int len = seg->n, pad = 0;
      float *buf = NULL;
      if (s->ring && from <= seg->start && to >= seg_end && (buf = malloc(sizeof(float) * (to - from)))) {
        for (long long i = from; i < to; i++) buf[i - from] = s->ring[i % RING];
        audio = buf;
        len = (int)(to - from);
        pad = (int)(seg->start - from);
      }
      const SherpaOnnxOfflineStream *os = p_SherpaOnnxCreateOfflineStream(g_rec);
      p_SherpaOnnxAcceptWaveformOffline(os, VAD_RATE, audio, len);
      p_SherpaOnnxDecodeOfflineStream(g_rec, os);
      const SherpaOnnxOfflineRecognizerResult *r = p_SherpaOnnxGetOfflineStreamResult(os);
      if (r && r->text && r->text[0]) {
        // A phrase as long as max_speech was cut by the limit, not by a pause.
        int complete = seg->n < (int)((T.max_speech - 0.3f) * VAD_RATE);
        char *text = postprocess(r->text, complete);
        if (T.dump) dump_phrase(audio, len, text);
        char *rest = dedup(stream, text);
        remember(stream, text);  // repeats still count as said, for the other stream
        if (rest) {
          logf_("dedup: stream %d repeat%s: %s", stream, rest[0] ? ", kept new part" : " dropped", text);
          free(text);
          text = rest[0] ? rest : NULL;
          if (!text) free(rest);
        }
        if (text) {
          out[n].text = text;
          out[n].start_us = (long long)(seg->start - pad) * 1000000 / VAD_RATE;
          out[n].end_us = (long long)(seg->start + seg->n) * 1000000 / VAD_RATE;
          n++;
        }
      }
      if (r) p_SherpaOnnxDestroyOfflineRecognizerResult(r);
      p_SherpaOnnxDestroyOfflineStream(os);
      free(buf);
    }
    p_SherpaOnnxDestroySpeechSegment(seg);
    p_SherpaOnnxVoiceActivityDetectorPop(s->vad);
  }
  return n;
}

int sv_push(int stream, const float *samples, int n, SvResult *out, int max_out) {
  if (!wait_ready()) return -1;
  pthread_mutex_lock(&g_mu);
  Stream *s = get_stream(stream);
  const SherpaOnnxResampleOut *ro = p_SherpaOnnxLinearResamplerResample(s->resampler, samples, n, 0);
  if (ro && ro->n > 0) {
    ring_push(s, ro->samples, ro->n);
    p_SherpaOnnxVoiceActivityDetectorAcceptWaveform(s->vad, ro->samples, ro->n);
  }
  if (ro) p_SherpaOnnxLinearResamplerResampleFree(ro);
  int got = drain(stream, s, out, max_out);
  pthread_mutex_unlock(&g_mu);
  return got;
}

int sv_finish(int stream, SvResult *out, int max_out) {
  if (!wait_ready()) return -1;
  pthread_mutex_lock(&g_mu);
  Stream *s = get_stream(stream);
  p_SherpaOnnxVoiceActivityDetectorFlush(s->vad);
  int got = drain(stream, s, out, max_out);
  // The next session starts from zero: timestamps are relative to stream start.
  p_SherpaOnnxVoiceActivityDetectorReset(s->vad);
  p_SherpaOnnxLinearResamplerReset(s->resampler);
  s->fed = 0;
  pthread_mutex_unlock(&g_mu);
  return got;
}
