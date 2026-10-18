#!/bin/zsh
# Builds libfarsight.dylib (arm64).
cd ${0:A:h} && clang -arch arm64 -dynamiclib -O2 -Wall -o libfarsight.dylib farsight.c sensevoice_engine.c persian_itn.c persian_text.c persian_font.c chat_bridge.c cJSON.c -lcurl -framework CoreText -framework CoreFoundation
