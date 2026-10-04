#!/usr/bin/env bash
set -ex

if [ ! -f ../scripts/kokoro/v1.1-zh/kokoro.onnx ]; then
  echo 'Export Kokoro v1.1-zh with pred_dur using scripts/kokoro/v1.1-zh/run.sh.' >&2
  exit 1
fi

cargo run --example kokoro_tts_zh_en
