// Copyright (c)  2025  Xiaomi Corporation
import 'dart:io';

import 'package:args/args.dart';
import 'package:sherpa_onnx/sherpa_onnx.dart' as sherpa_onnx;

import 'kokoro_input.dart';

void main(List<String> arguments) async {
  await sherpa_onnx.initBindingsAsync();

  final parser = ArgParser()
    ..addOption('model', help: 'Path to the onnx model')
    ..addOption('voices', help: 'Path to the voices.bin')
    ..addOption('tokens', help: 'Path to tokens.txt')
    ..addOption('g2p-output', help: 'Path to misaki-rs G2pOutput JSON')
    ..addOption('output-wav', help: 'Filename to save the generated audio')
    ..addOption('speed', help: 'Speech speed', defaultsTo: '1.0')
    ..addOption(
      'sid',
      help: 'Speaker ID to select. Used only for multi-speaker TTS',
      defaultsTo: '0',
    );
  final res = parser.parse(arguments);
  if (res['model'] == null ||
      res['voices'] == null ||
      res['tokens'] == null ||
      res['output-wav'] == null) {
    print(parser.usage);
    exit(1);
  }
  final model = res['model'] as String;
  final voices = res['voices'] as String;
  final tokens = res['tokens'] as String;
  final input = loadG2pOutput(res['g2p-output'] as String?, priceInput);
  final outputWav = res['output-wav'] as String;
  var speed = double.tryParse(res['speed'] as String) ?? 1.0;
  final sid = int.tryParse(res['sid'] as String) ?? 0;

  if (speed == 0) {
    speed = 1.0;
  }

  final kokoro = sherpa_onnx.OfflineTtsKokoroModelConfig(
    model: model,
    voices: voices,
    tokens: tokens,
  );

  final modelConfig = sherpa_onnx.OfflineTtsModelConfig(
    kokoro: kokoro,
    numThreads: 1,
    debug: true,
  );
  final config = sherpa_onnx.OfflineTtsConfig(
    model: modelConfig,
    maxNumSenetences: 1,
  );

  final tts = sherpa_onnx.OfflineTts(config);
  final genConfig = sherpa_onnx.OfflineTtsGenerationConfig(
    sid: sid,
    speed: speed,
    silenceScale: config.silenceScale,
  );
  final audio = tts.generateFromPhonemes(input: input, config: genConfig);
  tts.free();

  sherpa_onnx.writeWave(
    filename: outputWav,
    samples: audio.samples,
    sampleRate: audio.sampleRate,
  );
  printSpanAlignments(audio);
  print('Saved to $outputWav');
}
