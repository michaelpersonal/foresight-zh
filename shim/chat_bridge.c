// Chat bridge for the foresight-zh shim.
//
// Foresight's chat calls LiteRT-LM's conversation C API (litert_lm_*) with JSON
// messages and runs Gemma 4 E2B. When the local OpenAI-compatible server is up
// (default: the shared qwen38-27b-gguf llama-server on :8096), conversations are
// served by it instead. When it is down, every call goes to the real API.
//
// Protocol (traced from Foresight 1.0.0):
//   create_conversation_v2(engine, config_json) -> conv
//     config_json: {"tools":[...], "messages":[...], "temperature", "top_k", ...}
//   send_message_stream(conv, message_json, cb, user_data)
//     cb(char *chunk_json, void *user_data); chunk is malloc'ed, the app frees it
//     with litert_lm_free_response_string. "" ends the stream.
//     chunk: {"role":"assistant","content":[{"type":"text","text":"..."}]}
//        or  {"role":"assistant","tool_calls":[{"type":"function","function":
//             {"name":"...","arguments":{...}}}]}
//   clone_conversation(conv), cancel_process(conv), delete_conversation(conv)
//
// Sampling is left to the server: its settings are tied to the model's template
// mode (see ~/local-llm runtime/config/llama-servers.zsh).

#include "chat_bridge.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"

extern void logf_(const char *fmt, ...);

#define MAX_TOKENS 2048
#define MAX_TOOL_CALLS 8

typedef void (*stream_cb)(char *chunk, void *user_data);
typedef void *(*create_v2_fn)(void *engine, const char *config_json);
typedef void *(*conv_fn)(void *conv);
typedef void (*conv_void_fn)(void *conv);
typedef int (*send_stream_fn)(void *conv, const char *msg, stream_cb cb, void *ud);
typedef char *(*send_fn)(void *conv, const char *msg);

static struct {
  create_v2_fn create_v2;
  conv_fn clone;
  conv_void_fn cancel, del;
  send_stream_fn send_stream;
  send_fn send;
} R;

typedef struct Conv {
  // First word NULL: a real litert_lm_* call handed one of our handles reads a
  // null instance and fails cleanly instead of crashing.
  void *null_instance;
  struct Conv *next;
  pthread_mutex_t mu;
  cJSON *messages;  // OpenAI-format history
  cJSON *tools;     // OpenAI-format tools, or NULL
  int refs;         // app handle + running requests
  volatile int cancel;
} Conv;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static Conv *g_convs;
static char g_url[512], g_model[128];
static cJSON *g_extra;  // extra request fields from the config line
static int g_trace_chat;
static char g_dump_dir[700];

// ---------------------------------------------------------------- handles ---

static Conv *find_conv(void *p) {
  pthread_mutex_lock(&g_mu);
  Conv *c = g_convs;
  while (c && c != p) c = c->next;
  pthread_mutex_unlock(&g_mu);
  return c;
}

static Conv *new_conv(cJSON *messages, cJSON *tools) {
  Conv *c = calloc(1, sizeof *c);
  pthread_mutex_init(&c->mu, NULL);
  c->messages = messages;
  c->tools = tools;
  c->refs = 1;
  pthread_mutex_lock(&g_mu);
  c->next = g_convs;
  g_convs = c;
  pthread_mutex_unlock(&g_mu);
  return c;
}

static void unref(Conv *c) {
  pthread_mutex_lock(&c->mu);
  int left = --c->refs;
  pthread_mutex_unlock(&c->mu);
  if (left) return;
  pthread_mutex_lock(&g_mu);
  for (Conv **pp = &g_convs; *pp; pp = &(*pp)->next)
    if (*pp == c) {
      *pp = c->next;
      break;
    }
  pthread_mutex_unlock(&g_mu);
  cJSON_Delete(c->messages);
  cJSON_Delete(c->tools);
  pthread_mutex_destroy(&c->mu);
  free(c);
}

// ------------------------------------------------------------- conversion ---

static int is_cjk(unsigned c) {
  return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
         (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFF);
}

// Decodes one UTF-8 code point and returns its byte length.
static int utf8_dec(const unsigned char *s, unsigned *c) {
  int n = s[0] < 0x80 ? 1 : s[0] < 0xE0 ? 2 : s[0] < 0xF0 ? 3 : 4;
  unsigned v = n == 1 ? s[0] : s[0] & (0xFF >> (n + 1));
  for (int i = 1; i < n; i++) {
    if (!s[i]) return i;
    v = (v << 6) | (s[i] & 0x3F);
  }
  *c = v;
  return n;
}

// The shim spaces Chinese transcripts with U+200A for Foresight's word counts,
// and Foresight turns those into plain spaces when it builds chat context. Drop
// every hair space, and any plain space between two CJK characters.
static char *unspace_cjk(char *s) {
  unsigned char *r = (unsigned char *)s, *w = r;
  unsigned prev = 0;
  while (*r) {
    unsigned c = 0;
    int n = utf8_dec(r, &c);
    if (c == 0x200A) {
      r += n;
      continue;
    }
    if (c == ' ' && is_cjk(prev)) {
      unsigned char *q = r;
      while (*q == ' ') q++;
      unsigned next = 0;
      if (*q) utf8_dec(q, &next);
      if ((unsigned char)q[0] == 0xE2 && q[1] == 0x80 && q[2] == 0x8A) next = prev;  // more hair spaces
      if (is_cjk(next)) {
        r = q;
        continue;
      }
    }
    memmove(w, r, n);
    w += n;
    r += n;
    prev = c;
  }
  *w = 0;
  return s;
}

// Arabic-script letters (Persian included); digits and punctuation excluded.
static int is_arabic_letter(unsigned c) {
  return (c >= 0x0620 && c <= 0x064A) || (c >= 0x066E && c <= 0x06D3) || (c >= 0xFB50 && c <= 0xFDFF) ||
         (c >= 0xFE70 && c <= 0xFEFF);
}

// Share of CJK (want_cjk) or Arabic-script characters among letters.
static double script_share(const char *s, int want_cjk) {
  long hit = 0, letters = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p;) {
    unsigned c = 0;
    p += utf8_dec(p, &c);
    int cjk = is_cjk(c), ar = is_arabic_letter(c);
    if (cjk || ar || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')) letters++;
    if (want_cjk ? cjk : ar) hit++;
  }
  return letters ? (double)hit / letters : 0;
}

// Joins the text parts of a LiteRT-LM message's content (string or part array).
static char *content_text(const cJSON *content) {
  if (cJSON_IsString(content)) return unspace_cjk(strdup(content->valuestring));
  size_t cap = 256, len = 0;
  char *s = malloc(cap);
  s[0] = 0;
  const cJSON *part;
  cJSON_ArrayForEach(part, content) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(part, "text");
    if (!cJSON_IsString(t)) continue;
    size_t n = strlen(t->valuestring);
    while (len + n + 2 > cap) s = realloc(s, cap *= 2);
    if (len) s[len++] = '\n';
    memcpy(s + len, t->valuestring, n + 1);
    len += n;
  }
  return unspace_cjk(s);
}

// Foresight stores earlier assistant replies as the JSON string {"text":"..."}.
static char *unwrap_text(char *s) {
  if (s[0] != '{') return s;
  cJSON *j = cJSON_Parse(s);
  const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "text");
  if (cJSON_IsString(t)) {
    char *u = strdup(t->valuestring);
    free(s);
    s = u;
  }
  cJSON_Delete(j);
  return s;
}

static cJSON *to_openai_message(const cJSON *m) {
  const cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
  if (!cJSON_IsString(role)) return NULL;
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "role", role->valuestring);
  char *text = content_text(cJSON_GetObjectItemCaseSensitive(m, "content"));
  if (strcmp(role->valuestring, "assistant") == 0) text = unwrap_text(text);
  cJSON_AddStringToObject(o, "content", text);
  free(text);
  const cJSON *calls = cJSON_GetObjectItemCaseSensitive(m, "tool_calls");
  if (cJSON_IsArray(calls)) {
    cJSON *oc = cJSON_AddArrayToObject(o, "tool_calls");
    int i = 0;
    const cJSON *call;
    cJSON_ArrayForEach(call, calls) {
      const cJSON *fn = cJSON_GetObjectItemCaseSensitive(call, "function");
      const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
      if (!cJSON_IsString(name)) continue;
      const cJSON *args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
      char *a = args ? (cJSON_IsString(args) ? strdup(args->valuestring) : cJSON_PrintUnformatted(args))
                     : strdup("{}");
      char id[32];
      snprintf(id, sizeof id, "call_%d", i++);
      cJSON *c = cJSON_CreateObject();
      cJSON_AddStringToObject(c, "id", id);
      cJSON_AddStringToObject(c, "type", "function");
      cJSON *f = cJSON_AddObjectToObject(c, "function");
      cJSON_AddStringToObject(f, "name", name->valuestring);
      cJSON_AddStringToObject(f, "arguments", a);
      free(a);
      cJSON_AddItemToArray(oc, c);
    }
  }
  return o;
}

static void strip_key(cJSON *j, const char *key) {
  if (!cJSON_IsObject(j) && !cJSON_IsArray(j)) return;
  cJSON_DeleteItemFromObjectCaseSensitive(j, key);
  cJSON *child;
  cJSON_ArrayForEach(child, j) strip_key(child, key);
}

static cJSON *to_openai_tools(const cJSON *tools) {
  if (!cJSON_IsArray(tools) || cJSON_GetArraySize(tools) == 0) return NULL;
  cJSON *out = cJSON_CreateArray();
  const cJSON *t;
  cJSON_ArrayForEach(t, tools) {
    cJSON *fn = cJSON_Duplicate(t, 1);
    strip_key(fn, "nullable");
    cJSON *w = cJSON_CreateObject();
    cJSON_AddStringToObject(w, "type", "function");
    cJSON_AddItemToObject(w, "function", fn);
    cJSON_AddItemToArray(out, w);
  }
  return out;
}

static char *text_chunk(const char *text) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "role", "assistant");
  cJSON *parts = cJSON_AddArrayToObject(o, "content");
  cJSON *p = cJSON_CreateObject();
  cJSON_AddStringToObject(p, "type", "text");
  cJSON_AddStringToObject(p, "text", text);
  cJSON_AddItemToArray(parts, p);
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  return s;
}

static char *empty_chunk(void) { return calloc(1, 1); }

// ------------------------------------------------------------------ HTTP ---

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static void curl_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

static int g_health_ok;
static time_t g_health_at;

typedef struct { char buf[8192]; size_t len; } Body;

static size_t keep_body(void *p, size_t s, size_t n, void *u) {
  Body *b = u;
  size_t k = s * n, room = sizeof b->buf - 1 - b->len;
  if (k < room) room = k;
  memcpy(b->buf + b->len, p, room);
  b->len += room;
  b->buf[b->len] = 0;
  return s * n;
}

static int server_up(void) {
  time_t now = time(NULL);
  pthread_mutex_lock(&g_mu);
  if (now - g_health_at < 5) {
    int ok = g_health_ok;
    pthread_mutex_unlock(&g_mu);
    return ok;
  }
  pthread_mutex_unlock(&g_mu);
  pthread_once(&g_curl_once, curl_init);
  char url[600];
  snprintf(url, sizeof url, "%s/models", g_url);
  CURL *h = curl_easy_init();
  curl_easy_setopt(h, CURLOPT_URL, url);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 300L);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 1500L);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  Body body = {.len = 0};
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, keep_body);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
  long code = 0;
  int ok = curl_easy_perform(h) == CURLE_OK &&
           curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code) == CURLE_OK && code == 200;
  // Another engine can hold the same port (Splash and ds4 both use :8000), so the
  // configured model has to be the one listed.
  if (ok && g_model[0] && !strstr(body.buf, g_model)) ok = 0;
  curl_easy_cleanup(h);
  pthread_mutex_lock(&g_mu);
  if (ok != g_health_ok || !g_health_at) logf_("chat: %s %s", g_url, ok ? "up -> Qwen" : "down -> Gemma");
  g_health_ok = ok;
  g_health_at = now;
  pthread_mutex_unlock(&g_mu);
  return ok;
}

typedef struct {
  Conv *conv;
  stream_cb cb;
  void *ud;
  char *line;  // SSE line buffer
  size_t line_len, line_cap;
  char *text;  // full reply text
  size_t text_len, text_cap;
  struct { char *name, *args; size_t args_len; } calls[MAX_TOOL_CALLS];
  int ncalls;
  int chunks;
  char err[256];
} Req;

static void append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
  if (*len + n + 1 > *cap) {
    *cap = (*len + n + 1) * 2;
    *buf = realloc(*buf, *cap);
  }
  memcpy(*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = 0;
}

static void handle_event(Req *r, const char *data) {
  if (strcmp(data, "[DONE]") == 0) return;
  cJSON *j = cJSON_Parse(data);
  if (!j) return;
  const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
  if (e) {
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(e, "message");
    snprintf(r->err, sizeof r->err, "%s", cJSON_IsString(msg) ? msg->valuestring : "server error");
  }
  const cJSON *ch = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "choices"), 0);
  const cJSON *delta = cJSON_GetObjectItemCaseSensitive(ch, "delta");
  const cJSON *content = cJSON_GetObjectItemCaseSensitive(delta, "content");
  if (cJSON_IsString(content) && content->valuestring[0]) {
    size_t n = strlen(content->valuestring);
    append(&r->text, &r->text_len, &r->text_cap, content->valuestring, n);
    if (r->cb) {
      r->cb(text_chunk(content->valuestring), r->ud);
      r->chunks++;
    }
  }
  const cJSON *tc;
  cJSON_ArrayForEach(tc, cJSON_GetObjectItemCaseSensitive(delta, "tool_calls")) {
    const cJSON *idx = cJSON_GetObjectItemCaseSensitive(tc, "index");
    int i = cJSON_IsNumber(idx) ? idx->valueint : 0;
    if (i < 0 || i >= MAX_TOOL_CALLS) continue;
    if (i >= r->ncalls) r->ncalls = i + 1;
    const cJSON *fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
    if (cJSON_IsString(name) && !r->calls[i].name) r->calls[i].name = strdup(name->valuestring);
    if (cJSON_IsString(args)) {
      size_t cap = r->calls[i].args ? r->calls[i].args_len + 1 : 0;
      append(&r->calls[i].args, &r->calls[i].args_len, &cap, args->valuestring, strlen(args->valuestring));
    }
  }
  cJSON_Delete(j);
}

static size_t on_data(void *p, size_t s, size_t n, void *u) {
  Req *r = u;
  if (r->conv->cancel) return 0;
  const char *d = p;
  size_t len = s * n;
  for (size_t i = 0; i < len; i++) {
    if (d[i] == '\n') {
      if (r->line_len > 6 && strncmp(r->line, "data: ", 6) == 0) handle_event(r, r->line + 6);
      else if (r->line_len && r->line[0] == '{') handle_event(r, r->line);  // non-SSE error body
      r->line_len = 0;
      if (r->line) r->line[0] = 0;
    } else if (d[i] != '\r') {
      append(&r->line, &r->line_len, &r->line_cap, &d[i], 1);
    }
  }
  return len;
}

// Foresight's meeting brief / notes prompts are English, so the model answers in
// English even for a Chinese or Persian transcript. Ask for the transcript's language,
// keeping the template headings the app expects.
static void match_transcript_language(cJSON *msgs) {
  const cJSON *last = NULL, *m;
  cJSON_ArrayForEach(m, msgs) {
    const cJSON *role = cJSON_GetObjectItemCaseSensitive(m, "role");
    if (cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0) last = m;
  }
  const cJSON *content = cJSON_GetObjectItemCaseSensitive(last, "content");
  if (!cJSON_IsString(content)) return;
  const char *t = content->valuestring;
  if (!strstr(t, "<meeting_transcript>") && !strstr(t, "<transcript_chunk>") && !strstr(t, "<new_transcript_section>"))
    return;
  static const char note_zh[] =
      "\n\nLanguage: the transcript is in Chinese. Write all of your output in Simplified Chinese. "
      "Keep the template's markdown headings (such as \"# Overview\" and \"# Details\") and any "
      "required JSON keys or field names exactly as specified, in English.";
  static const char note_fa[] =
      "\n\nLanguage: the transcript is in Persian (Farsi). Write all of your output in Persian. "
      "Keep the template's markdown headings (such as \"# Overview\" and \"# Details\") and any "
      "required JSON keys or field names exactly as specified, in English.";
  const char *note, *lang;
  if (script_share(t, 1) >= 0.3) note = note_zh, lang = "Chinese";
  else if (script_share(t, 0) >= 0.3) note = note_fa, lang = "Persian";
  else return;
  cJSON *first = cJSON_GetArrayItem(msgs, 0);
  const cJSON *role = cJSON_GetObjectItemCaseSensitive(first, "role");
  const cJSON *sys = cJSON_GetObjectItemCaseSensitive(first, "content");
  if (cJSON_IsString(role) && strcmp(role->valuestring, "system") == 0 && cJSON_IsString(sys)) {
    size_t n = strlen(sys->valuestring) + strlen(note) + 1;
    char *merged = malloc(n);
    snprintf(merged, n, "%s%s", sys->valuestring, note);
    cJSON_ReplaceItemInObjectCaseSensitive(first, "content", cJSON_CreateString(merged));
    free(merged);
  } else {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "role", "system");
    cJSON_AddStringToObject(o, "content", note + 2);
    cJSON_InsertItemInArray(msgs, 0, o);
  }
  logf_("chat: %s transcript -> asking for %s output", lang, lang);
}

// Runs one request against the server, appending the reply to the history.
static void run_request(Req *r) {
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  Conv *c = r->conv;
  cJSON *body = cJSON_CreateObject();
  if (g_model[0]) cJSON_AddStringToObject(body, "model", g_model);
  pthread_mutex_lock(&c->mu);
  cJSON *msgs = cJSON_Duplicate(c->messages, 1);
  cJSON_AddItemToObject(body, "messages", msgs);
  match_transcript_language(msgs);
  if (c->tools) cJSON_AddItemToObject(body, "tools", cJSON_Duplicate(c->tools, 1));
  int nmsg = cJSON_GetArraySize(c->messages);
  pthread_mutex_unlock(&c->mu);
  cJSON_AddBoolToObject(body, "stream", 1);
  cJSON_AddNumberToObject(body, "max_tokens", MAX_TOKENS);
  const cJSON *x;
  cJSON_ArrayForEach(x, g_extra) {
    cJSON_DeleteItemFromObjectCaseSensitive(body, x->string);
    cJSON_AddItemToObject(body, x->string, cJSON_Duplicate(x, 1));
  }
  char *json = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  if (g_trace_chat && g_dump_dir[0]) {
    static int seq;
    char p[800];
    snprintf(p, sizeof p, "%s/chat-%04d.json", g_dump_dir, __sync_add_and_fetch(&seq, 1));
    FILE *f = fopen(p, "w");
    if (f) {
      fputs(json, f);
      fclose(f);
    }
  }

  pthread_once(&g_curl_once, curl_init);
  char url[600];
  snprintf(url, sizeof url, "%s/chat/completions", g_url);
  CURL *h = curl_easy_init();
  struct curl_slist *hdr = curl_slist_append(NULL, "Content-Type: application/json");
  curl_easy_setopt(h, CURLOPT_URL, url);
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, json);
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, r);
  CURLcode rc = curl_easy_perform(h);
  long code = 0;
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(hdr);
  curl_easy_cleanup(h);
  free(json);
  if (r->line_len && r->line[0] == '{') handle_event(r, r->line);  // unterminated body

  if (rc != CURLE_OK && !c->cancel && !r->err[0]) snprintf(r->err, sizeof r->err, "%s", curl_easy_strerror(rc));
  else if (code != 200 && !r->err[0] && !c->cancel) snprintf(r->err, sizeof r->err, "HTTP %ld", code);

  // Tool calls go out as one chunk at the end, in Foresight's format.
  cJSON *calls_out = NULL, *calls_hist = NULL;
  if (r->ncalls) {
    calls_out = cJSON_CreateArray();
    calls_hist = cJSON_CreateArray();
    for (int i = 0; i < r->ncalls; i++) {
      if (!r->calls[i].name) continue;
      cJSON *args = r->calls[i].args ? cJSON_Parse(r->calls[i].args) : NULL;
      if (!args) args = cJSON_CreateObject();
      cJSON *c1 = cJSON_CreateObject();
      cJSON_AddStringToObject(c1, "type", "function");
      cJSON *f1 = cJSON_AddObjectToObject(c1, "function");
      cJSON_AddStringToObject(f1, "name", r->calls[i].name);
      cJSON_AddItemToObject(f1, "arguments", cJSON_Duplicate(args, 1));
      cJSON_AddItemToArray(calls_out, c1);
      char id[32];
      snprintf(id, sizeof id, "call_%d", i);
      cJSON *c2 = cJSON_CreateObject();
      cJSON_AddStringToObject(c2, "id", id);
      cJSON_AddStringToObject(c2, "type", "function");
      cJSON *f2 = cJSON_AddObjectToObject(c2, "function");
      cJSON_AddStringToObject(f2, "name", r->calls[i].name);
      char *a = cJSON_PrintUnformatted(args);
      cJSON_AddStringToObject(f2, "arguments", a);
      free(a);
      cJSON_AddItemToArray(calls_hist, c2);
      cJSON_Delete(args);
    }
  }

  cJSON *hist = cJSON_CreateObject();
  cJSON_AddStringToObject(hist, "role", "assistant");
  cJSON_AddStringToObject(hist, "content", r->text ? r->text : "");
  if (calls_hist) cJSON_AddItemToObject(hist, "tool_calls", calls_hist);
  pthread_mutex_lock(&c->mu);
  cJSON_AddItemToArray(c->messages, hist);
  pthread_mutex_unlock(&c->mu);

  if (r->cb) {
    if (calls_out && cJSON_GetArraySize(calls_out)) {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "role", "assistant");
      cJSON_AddItemToObject(o, "tool_calls", calls_out);
      calls_out = NULL;
      r->cb(cJSON_PrintUnformatted(o), r->ud);
      cJSON_Delete(o);
    } else if (r->err[0] && !r->chunks) {
      char msg[320];
      snprintf(msg, sizeof msg, "(Local model error: %s)", r->err);
      r->cb(text_chunk(msg), r->ud);
    }
    r->cb(empty_chunk(), r->ud);
  }
  cJSON_Delete(calls_out);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  logf_("chat: %d msgs -> %zu chars, %d tool calls, %.1fs%s%s%s", nmsg, r->text_len, r->ncalls,
        (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9, c->cancel ? " (cancelled)" : "",
        r->err[0] ? " error: " : "", r->err);
  if (g_trace_chat && r->text) logf_("chat reply: %s", r->text);
}

static void free_req(Req *r) {
  free(r->line);
  free(r->text);
  for (int i = 0; i < MAX_TOOL_CALLS; i++) {
    free(r->calls[i].name);
    free(r->calls[i].args);
  }
  free(r);
}

static void *request_thread(void *arg) {
  Req *r = arg;
  run_request(r);
  Conv *c = r->conv;
  free_req(r);
  unref(c);
  return NULL;
}

// Parses an app message and appends it to the history.
static int push_message(Conv *c, const char *msg_json) {
  cJSON *m = cJSON_Parse(msg_json);
  cJSON *o = m ? to_openai_message(m) : NULL;
  cJSON_Delete(m);
  if (!o) return 0;
  pthread_mutex_lock(&c->mu);
  cJSON_AddItemToArray(c->messages, o);
  c->cancel = 0;
  pthread_mutex_unlock(&c->mu);
  return 1;
}

// ------------------------------------------------------------ replacements ---

static void *my_create_v2(void *engine, const char *config_json) {
  if (!config_json || !server_up()) return R.create_v2(engine, config_json);
  cJSON *cfg = cJSON_Parse(config_json);
  if (!cfg) return R.create_v2(engine, config_json);
  cJSON *msgs = cJSON_CreateArray();
  const cJSON *m;
  cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(cfg, "messages")) {
    cJSON *o = to_openai_message(m);
    if (o) cJSON_AddItemToArray(msgs, o);
  }
  cJSON *tools = to_openai_tools(cJSON_GetObjectItemCaseSensitive(cfg, "tools"));
  cJSON_Delete(cfg);
  return new_conv(msgs, tools);
}

static void *my_clone(void *conv) {
  Conv *c = find_conv(conv);
  if (!c) return R.clone(conv);
  pthread_mutex_lock(&c->mu);
  cJSON *msgs = cJSON_Duplicate(c->messages, 1);
  cJSON *tools = c->tools ? cJSON_Duplicate(c->tools, 1) : NULL;
  pthread_mutex_unlock(&c->mu);
  return new_conv(msgs, tools);
}

static void my_cancel(void *conv) {
  Conv *c = find_conv(conv);
  if (!c) {
    R.cancel(conv);
    return;
  }
  c->cancel = 1;
}

static void my_delete(void *conv) {
  Conv *c = find_conv(conv);
  if (!c) {
    R.del(conv);
    return;
  }
  c->cancel = 1;
  unref(c);
}

static int my_send_stream(void *conv, const char *msg, stream_cb cb, void *ud) {
  Conv *c = find_conv(conv);
  if (!c) return R.send_stream(conv, msg, cb, ud);
  if (!msg || !push_message(c, msg)) {
    cb(empty_chunk(), ud);
    return 0;
  }
  Req *r = calloc(1, sizeof *r);
  r->conv = c;
  r->cb = cb;
  r->ud = ud;
  pthread_mutex_lock(&c->mu);
  c->refs++;
  pthread_mutex_unlock(&c->mu);
  pthread_t t;
  pthread_create(&t, NULL, request_thread, r);
  pthread_detach(t);
  return 0;
}

// Non-streaming variant: same streamed request, reply returned as one JSON string.
static char *my_send(void *conv, const char *msg) {
  Conv *c = find_conv(conv);
  if (!c) return R.send(conv, msg);
  if (!msg || !push_message(c, msg)) return text_chunk("");
  Req *r = calloc(1, sizeof *r);
  r->conv = c;
  run_request(r);
  char *out = text_chunk(r->text ? r->text : "");
  free_req(r);
  return out;
}

// ----------------------------------------------------------------- config ---

void chat_bridge_init(const char *home, int trace) {
  g_trace_chat = trace;
  snprintf(g_dump_dir, sizeof g_dump_dir, "%s/Library/Logs/foresight-zh/lm", home);
  if (trace) mkdir(g_dump_dir, 0755);
  snprintf(g_url, sizeof g_url, "http://127.0.0.1:8096/v1");
  snprintf(g_model, sizeof g_model, "qwen3.8-27b-mtp");
  char p[1024], line[1200] = "";
  const char *env = getenv("FORESIGHT_ZH_CHAT");
  if (env) {
    snprintf(line, sizeof line, "%s", env);
  } else {
    snprintf(p, sizeof p, "%s/tools/foresight-zh/chat", home);
    FILE *f = fopen(p, "r");
    if (f) {
      if (fgets(line, sizeof line, f)) line[strcspn(line, "\r\n")] = 0;
      fclose(f);
    }
  }
  if (strcmp(line, "off") == 0) {
    g_url[0] = 0;
    logf_("chat: bridge off (Gemma)");
    return;
  }
  // "<base_url> [model] [{extra request fields}]"
  if (line[0]) {
    char url[512] = "", model[128] = "";
    int used = 0;
    sscanf(line, "%511s %127s %n", url, model, &used);
    snprintf(g_url, sizeof g_url, "%s", url);
    snprintf(g_model, sizeof g_model, "%s", model[0] == '{' ? "" : model);
    const char *extra = model[0] == '{' ? strchr(line, '{') : (used ? line + used : "");
    if (extra && *extra) {
      g_extra = cJSON_Parse(extra);
      if (!cJSON_IsObject(g_extra)) {
        logf_("chat: ignoring malformed extra fields: %s", extra);
        cJSON_Delete(g_extra);
        g_extra = NULL;
      }
    }
  }
  char *x = g_extra ? cJSON_PrintUnformatted(g_extra) : NULL;
  logf_("chat: bridge -> %s (%s)%s%s, Gemma when unreachable", g_url, g_model[0] ? g_model : "default model",
        x ? " extra " : "", x ? x : "");
  free(x);
}

void *chat_bridge_wrap(const char *symbol, void *real) {
  if (!g_url[0]) return NULL;
#define HOOK(name, field, fn)                \
  if (strcmp(symbol, name) == 0) {           \
    R.field = (__typeof__(R.field))real;     \
    return (void *)fn;                       \
  }
  HOOK("litert_lm_create_conversation_v2", create_v2, my_create_v2);
  HOOK("litert_lm_clone_conversation", clone, my_clone);
  HOOK("litert_lm_conversation_cancel_process", cancel, my_cancel);
  HOOK("litert_lm_delete_conversation", del, my_delete);
  HOOK("litert_lm_conversation_send_message_stream", send_stream, my_send_stream);
  HOOK("litert_lm_conversation_send_message", send, my_send);
  return NULL;
}
