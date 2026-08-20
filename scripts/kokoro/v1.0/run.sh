#!/usr/bin/env bash
# Copyright    2025  Xiaomi Corp.        (authors: Fangjun Kuang)

set -ex

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$script_dir"

output_dir=./kokoro-multi-lang-v1_0

if [ ! -d ./Kokoro-82M ]; then
  git clone https://huggingface.co/hexgrad/Kokoro-82M
fi

# https://huggingface.co/hexgrad/Kokoro-82M/tree/main/voices
#
# af -> American female
# am -> American male
# bf -> British female
# bm -> British male

if [ ! -f ./kokoro.onnx ]; then
  python3 ./export_onnx.py
fi


if [ ! -f ./.add-meta-data.done ]; then
  python3 ./add_meta_data.py
  touch ./.add-meta-data.done
fi

if [ ! -f ./kokoro.int8.onnx ]; then
  python3 ./dynamic_quantization.py
fi

if [ ! -f us_gold.json ]; then
  curl -SL -O https://raw.githubusercontent.com/hexgrad/misaki/refs/heads/main/misaki/data/us_gold.json
fi

if [ ! -f us_silver.json ]; then
  curl -SL -O https://raw.githubusercontent.com/hexgrad/misaki/refs/heads/main/misaki/data/us_silver.json
fi

if [ ! -f gb_gold.json ]; then
  curl -SL -O https://raw.githubusercontent.com/hexgrad/misaki/refs/heads/main/misaki/data/gb_gold.json
fi

if [ ! -f gb_silver.json ]; then
  curl -SL -O https://raw.githubusercontent.com/hexgrad/misaki/refs/heads/main/misaki/data/gb_silver.json
fi

if [ ! -f ./tokens.txt ]; then
  ./generate_tokens.py
fi

if [ ! -f ./lexicon-zh.txt ]; then
  ./generate_lexicon_zh.py
fi

if [[ ! -f ./lexicon-us-en.txt || ! -f ./lexicon-gb-en.txt ]]; then
  ./generate_lexicon_en.py
fi

if [ ! -f ./voices.bin ]; then
  ./generate_voices_bin.py
fi

./test.py

# Package the generated model artifacts together with the complete eSpeak NG
# runtime data required by sherpa-onnx. The CMake build's data directory only
# contains language assets; piper_phonemize ships the compiled phontab and
# related files that the runtime validates.
mkdir -p "$output_dir/espeak-ng-data"
cp -a \
  .venv/lib/python3.12/site-packages/piper_phonemize/espeak-ng-data/. \
  "$output_dir/espeak-ng-data/"

for generated_file in \
  kokoro.onnx \
  kokoro.int8.onnx \
  tokens.txt \
  lexicon-zh.txt \
  lexicon-us-en.txt \
  lexicon-gb-en.txt \
  voices.bin \
  kokoro_*.wav; do
  if [ -e "$generated_file" ]; then
    mv -v "$generated_file" "$output_dir/"
  fi
done

ls -lh "$output_dir"
