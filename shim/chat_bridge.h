#pragma once

// Reads ~/tools/farsight/chat ("off", or "<base_url> [model]").
void chat_bridge_init(const char *home, int trace);
// Returns a replacement for a litert_lm_* chat symbol, or NULL to leave it alone.
void *chat_bridge_wrap(const char *symbol, void *real);
