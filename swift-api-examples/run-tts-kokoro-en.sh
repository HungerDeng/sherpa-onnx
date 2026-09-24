#!/usr/bin/env bash

set -ex

if [ ! -d ../build-macos ]; then
  echo "Please run ../build-macos.sh first!"
  exit 1
fi

if [ ! -f ./kokoro-multi-lang-v1_0/model.onnx ]; then
  echo "Provide a Kokoro v1.0 model exported with pred_dur at ./kokoro-multi-lang-v1_0/"
  exit 1
fi

if [ ! -e ./tts-kokoro-en ] || [ ../build-macos/install/lib/libsherpa-onnx-c-api.a -nt ./tts-kokoro-en ]; then
  # Note: We use -lc++ to link against libc++ instead of libstdc++
  swiftc \
    -lc++ \
    -I ../build-macos/install/include \
    -import-objc-header ./SherpaOnnx-Bridging-Header.h \
    ./tts-kokoro-en.swift  ./SherpaOnnx.swift \
    -L ../build-macos/install/lib/ \
    -l sherpa-onnx-c-api \
    -l onnxruntime \
    -o tts-kokoro-en

  strip tts-kokoro-en
else
  echo "./tts-kokoro-en exists - skip building"
fi

export DYLD_LIBRARY_PATH=$PWD/../build-macos/install/lib:$DYLD_LIBRARY_PATH
./tts-kokoro-en
