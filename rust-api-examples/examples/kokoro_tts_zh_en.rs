// Copyright (c) 2026 Xiaomi Corporation
//
// This file demonstrates how to use Kokoro TTS with sherpa-onnx's Rust API
// for offline phoneme-to-speech using precomputed misaki-rs output.

use sherpa_onnx::{
    GenerationConfig, OfflineTts, OfflineTtsConfig, OfflineTtsKokoroModelConfig, PhonemeInput,
    PhonemeSpan,
};
use std::time::Instant;

fn main() {
    let config = OfflineTtsConfig {
        model: sherpa_onnx::OfflineTtsModelConfig {
            kokoro: OfflineTtsKokoroModelConfig {
                model: Some("../scripts/kokoro/v1.1-zh/kokoro.onnx".into()),
                voices: Some("../scripts/kokoro/v1.1-zh/voices.bin".into()),
                tokens: Some("../scripts/kokoro/v1.1-zh/tokens.txt".into()),
                length_scale: 1.0,
                ..Default::default()
            },
            num_threads: 2,
            debug: false,
            ..Default::default()
        },
        ..Default::default()
    };

    let tts = OfflineTts::create(&config).expect("Failed to create OfflineTts");

    println!("Sample rate: {}", tts.sample_rate());
    println!("Num speakers: {}", tts.num_speakers());

    // The fourth span represents the multiword pronunciation of "$1,200".
    let input = PhonemeInput {
        phonemes: "ðə pɹˈaɪs ɪz wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz. ɐ bˈɪt ɛkspˈɛnsɪv.".into(),
        spans: [
            "ðə",
            "pɹˈaɪs",
            "ɪz",
            "wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz",
            ".",
            "ɐ",
            "bˈɪt",
            "ɛkspˈɛnsɪv",
            ".",
        ]
        .into_iter()
        .map(|phonemes| PhonemeSpan {
            phonemes: phonemes.into(),
        })
        .collect(),
    };

    let gen_config = GenerationConfig {
        sid: 0,
        speed: 1.0,
        ..Default::default()
    };

    let start = Instant::now();

    let audio = tts
        .generate_from_phonemes_with_config(
            &input,
            &gen_config,
            Some(|_samples: &[f32], progress: f32| -> bool {
                println!("Progress: {:.1}%", progress * 100.0);
                true
            }),
        )
        .expect("Generation failed");

    for alignment in audio.span_alignments().expect("Kokoro span alignments") {
        println!(
            "{} -> {}: {:.3}s to {:.3}s",
            alignment.original_phonemes,
            alignment.inferred_phonemes,
            alignment.start_ts,
            alignment.end_ts
        );
    }

    let elapsed_seconds = start.elapsed().as_secs_f32();
    let duration = audio.samples().len() as f32 / audio.sample_rate() as f32;
    let rtf = elapsed_seconds / duration;

    println!("Number of threads: {}", config.model.num_threads);
    println!("Elapsed seconds: {:.3} s", elapsed_seconds);
    println!("Audio duration: {:.3} s", duration);
    println!(
        "Real-time factor (RTF): {:.3}/{:.3} = {:.3}",
        elapsed_seconds, duration, rtf
    );

    let filename = "./generated-kokoro-zh-en-rust.wav";
    if audio.save(filename) {
        println!("Saved to: {}", filename);
    } else {
        eprintln!("Failed to save {}", filename);
    }
}
