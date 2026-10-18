#pragma once

typedef struct {
  char *text;          // malloc'ed; ownership passes to the result list
  long long start_us;  // relative to stream start
  long long end_us;
} SvResult;

enum { SV_LANG_SENSEVOICE, SV_LANG_PERSIAN };

// Selects the model (SV_LANG_*) and returns 1 if it, the VAD model and the
// sherpa-onnx library are installed.
int sv_available(const char *home, int lang);
// Loads sherpa-onnx and the models on a background thread.
void sv_start_loading(void);
// Feeds 48 kHz mono float audio for a stream; writes finished phrases to out.
// Returns the number of results, or -1 if the engine failed to load.
int sv_push(int stream, const float *samples, int n, SvResult *out, int max_out);
// Flushes the stream at the end of a session and resets it.
int sv_finish(int stream, SvResult *out, int max_out);
