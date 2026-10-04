// Copyright (c) 2026 Xiaomi Corporation
import 'dart:convert';
import 'dart:io';

import 'package:sherpa_onnx/sherpa_onnx.dart' as sherpa_onnx;

// These two inputs are the phonemes and span.phonemes from misaki-rs outputs.
const priceInput = sherpa_onnx.PhonemeInput(
  phonemes:
      'ðə pɹˈaɪs ɪz wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz. ɐ bˈɪt ɛkspˈɛnsɪv.',
  spans: [
    sherpa_onnx.PhonemeSpan(phonemes: 'ðə'),
    sherpa_onnx.PhonemeSpan(phonemes: 'pɹˈaɪs'),
    sherpa_onnx.PhonemeSpan(phonemes: 'ɪz'),
    sherpa_onnx.PhonemeSpan(phonemes: 'wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz'),
    sherpa_onnx.PhonemeSpan(phonemes: '.'),
    sherpa_onnx.PhonemeSpan(phonemes: 'ɐ'),
    sherpa_onnx.PhonemeSpan(phonemes: 'bˈɪt'),
    sherpa_onnx.PhonemeSpan(phonemes: 'ɛkspˈɛnsɪv'),
    sherpa_onnx.PhonemeSpan(phonemes: '.'),
  ],
);

const aiInput = sherpa_onnx.PhonemeInput(
  phonemes: 'ˌeɪˈaɪ ɪz sˌoʊ ˈɔːsʌm. aɪ kˈænt lˈɪv wɪðˈaʊt ɪt.',
  spans: [
    sherpa_onnx.PhonemeSpan(phonemes: 'ˌeɪˈaɪ'),
    sherpa_onnx.PhonemeSpan(phonemes: 'ɪz'),
    sherpa_onnx.PhonemeSpan(phonemes: 'sˌoʊ'),
    sherpa_onnx.PhonemeSpan(phonemes: 'ˈɔːsʌm'),
    sherpa_onnx.PhonemeSpan(phonemes: '.'),
    sherpa_onnx.PhonemeSpan(phonemes: 'aɪ'),
    sherpa_onnx.PhonemeSpan(phonemes: 'kˈænt'),
    sherpa_onnx.PhonemeSpan(phonemes: 'lˈɪv'),
    sherpa_onnx.PhonemeSpan(phonemes: 'wɪðˈaʊt'),
    sherpa_onnx.PhonemeSpan(phonemes: 'ɪt'),
    sherpa_onnx.PhonemeSpan(phonemes: '.'),
  ],
);

/// Keep only the fields consumed by Kokoro from a full misaki-rs G2pOutput.
sherpa_onnx.PhonemeInput loadG2pOutput(
  String? path,
  sherpa_onnx.PhonemeInput example,
) {
  if (path == null) return example;
  final value = jsonDecode(File(path).readAsStringSync());
  if (value is! Map || value['phonemes'] is! String || value['spans'] is! List) {
    throw const FormatException('Expected G2pOutput phonemes and spans');
  }
  final spans = <sherpa_onnx.PhonemeSpan>[];
  for (final span in value['spans'] as List) {
    if (span is! Map || span['phonemes'] is! String) {
      throw const FormatException('Expected spans[].phonemes strings');
    }
    spans.add(sherpa_onnx.PhonemeSpan(phonemes: span['phonemes'] as String));
  }
  return sherpa_onnx.PhonemeInput(
    phonemes: value['phonemes'] as String,
    spans: spans,
  );
}

void printSpanAlignments(sherpa_onnx.GeneratedAudio audio) {
  final alignments = audio.spanAlignments;
  if (alignments == null) return;
  for (final span in alignments) {
    print('${span.startTs.toStringAsFixed(3)}-'
        '${span.endTs.toStringAsFixed(3)} '
        '${span.originalPhonemes} -> ${span.inferredPhonemes}');
  }
}
