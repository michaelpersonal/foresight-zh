#!/bin/zsh
# Builds libforesight_zh.dylib (arm64).
cd ${0:A:h} && clang -arch arm64 -dynamiclib -O2 -Wall -o libforesight_zh.dylib foresight_zh.c sensevoice_engine.c persian_itn.c chat_bridge.c cJSON.c -lcurl
