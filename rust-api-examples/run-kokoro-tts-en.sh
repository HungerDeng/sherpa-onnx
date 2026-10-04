#!/usr/bin/env bash
set -ex

if [ ! -f ../scripts/kokoro/v1.0/kokoro.onnx ]; then
  echo 'Export Kokoro v1.0 with pred_dur using scripts/kokoro/v1.0/run.sh.' >&2
  exit 1
fi

cargo run --example kokoro_tts_en
