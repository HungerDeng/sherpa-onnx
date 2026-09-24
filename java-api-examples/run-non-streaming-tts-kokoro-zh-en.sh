#!/usr/bin/env bash

set -ex

source ./setup.sh

if [ ! -f ./kokoro-multi-lang-v1_0/model.onnx ]; then
  echo 'Export Kokoro v1.0 with pred_dur using scripts/kokoro/v1.0/run.sh and place its model.onnx, voices.bin, and tokens.txt in java-api-examples/kokoro-multi-lang-v1_0.' >&2
  exit 1
fi

java \
  -Dsherpa_onnx.native.path=$PWD/../build/lib \
  -cp ../sherpa-onnx/java-api/target/sherpa-onnx-jvm-*.jar \
  NonStreamingTtsKokoroZhEn.java
