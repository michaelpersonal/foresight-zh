// SenseVoice transcription engine for the foresight-zh shim.
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

#include "persian_itn.h"
#include "sherpa-onnx-c-api.h"

#define IN_RATE 48000
#define VAD_RATE 16000
#define MAX_STREAMS 4

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
    c.model_config.num_threads = 2;
    c.model_config.provider = "cpu";
    c.decoding_method = "greedy_search";
    g_rec = p_SherpaOnnxCreateOfflineRecognizer(&c);
    if (!g_rec) logf_("sensevoice: recognizer creation failed");
    ok = g_rec != NULL;
  }
  if (ok) {
    memset(&g_vad_cfg, 0, sizeof g_vad_cfg);
    g_vad_cfg.silero_vad.model = g_vad_model;
    g_vad_cfg.silero_vad.threshold = 0.5f;
    g_vad_cfg.silero_vad.min_silence_duration = 0.6f;   // pause that ends a phrase
    g_vad_cfg.silero_vad.min_speech_duration = 0.25f;
    g_vad_cfg.silero_vad.max_speech_duration = 15.0f;   // force a break in long monologues
    g_vad_cfg.silero_vad.window_size = 512;
    g_vad_cfg.sample_rate = VAD_RATE;
    g_vad_cfg.num_threads = 1;
    g_vad_cfg.provider = "cpu";
  }
  pthread_mutex_lock(&g_mu);
  g_state = ok ? 1 : -1;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);
  logf_("asr (%s): %s", g_lang == SV_LANG_PERSIAN ? "persian" : "sensevoice", ok ? "ready" : "unavailable");
  return NULL;
}

int sv_available(const char *home, int lang) {
  g_lang = lang;
  snprintf(g_dir, sizeof g_dir, "%s/local-llm/models/special/foresight-zh", home);
  if (lang == SV_LANG_PERSIAN) {
    snprintf(g_model, sizeof g_model, "%s/persian/shenava-rizeh-v1.0/model.onnx", g_dir);
    snprintf(g_tokens, sizeof g_tokens, "%s/persian/shenava-rizeh-v1.0/tokens.txt", g_dir);
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
  }
  return s;
}

// Transcribes every finished VAD segment; returns the number of results written.
static int drain(Stream *s, SvResult *out, int max_out) {
  int n = 0;
  while (!p_SherpaOnnxVoiceActivityDetectorEmpty(s->vad)) {
    const SherpaOnnxSpeechSegment *seg = p_SherpaOnnxVoiceActivityDetectorFront(s->vad);
    if (n < max_out && seg->n > 0) {
      const SherpaOnnxOfflineStream *os = p_SherpaOnnxCreateOfflineStream(g_rec);
      p_SherpaOnnxAcceptWaveformOffline(os, VAD_RATE, seg->samples, seg->n);
      p_SherpaOnnxDecodeOfflineStream(g_rec, os);
      const SherpaOnnxOfflineRecognizerResult *r = p_SherpaOnnxGetOfflineStreamResult(os);
      if (r && r->text && r->text[0]) {
        // Shenava spells numbers out; turn them into digits.
        out[n].text = g_lang == SV_LANG_PERSIAN ? persian_itn(r->text) : strdup(r->text);
        out[n].start_us = (long long)seg->start * 1000000 / VAD_RATE;
        out[n].end_us = (long long)(seg->start + seg->n) * 1000000 / VAD_RATE;
        n++;
      }
      if (r) p_SherpaOnnxDestroyOfflineRecognizerResult(r);
      p_SherpaOnnxDestroyOfflineStream(os);
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
  if (ro && ro->n > 0) p_SherpaOnnxVoiceActivityDetectorAcceptWaveform(s->vad, ro->samples, ro->n);
  if (ro) p_SherpaOnnxLinearResamplerResampleFree(ro);
  int got = drain(s, out, max_out);
  pthread_mutex_unlock(&g_mu);
  return got;
}

int sv_finish(int stream, SvResult *out, int max_out) {
  if (!wait_ready()) return -1;
  pthread_mutex_lock(&g_mu);
  Stream *s = get_stream(stream);
  p_SherpaOnnxVoiceActivityDetectorFlush(s->vad);
  int got = drain(s, out, max_out);
  // The next session starts from zero: timestamps are relative to stream start.
  p_SherpaOnnxVoiceActivityDetectorReset(s->vad);
  p_SherpaOnnxLinearResamplerReset(s->resampler);
  pthread_mutex_unlock(&g_mu);
  return got;
}
