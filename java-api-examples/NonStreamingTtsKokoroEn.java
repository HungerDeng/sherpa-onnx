// Copyright 2025 Xiaomi Corporation

// Kokoro synthesis from the phoneme fields of a Misaki G2pOutput.
import com.k2fsa.sherpa.onnx.*;

public class NonStreamingTtsKokoroEn {
  public static void main(String[] args) {
    String modelDir = "./kokoro-multi-lang-v1_0";
    PhonemeInput input =
        new PhonemeInput(
            "ðə pɹˈaɪs ɪz wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz. ɐ bˈɪt ɛkspˈɛnsɪv.",
            new PhonemeSpan[] {
              new PhonemeSpan("ðə"),
              new PhonemeSpan("pɹˈaɪs"),
              new PhonemeSpan("ɪz"),
              new PhonemeSpan("wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz"),
              new PhonemeSpan("."),
              new PhonemeSpan("ɐ"),
              new PhonemeSpan("bˈɪt"),
              new PhonemeSpan("ɛkspˈɛnsɪv"),
              new PhonemeSpan(".")
            });

    OfflineTtsKokoroModelConfig kokoroModelConfig =
        OfflineTtsKokoroModelConfig.builder()
            .setModel(modelDir + "/model.onnx")
            .setVoices(modelDir + "/voices.bin")
            .setTokens(modelDir + "/tokens.txt")
            .build();
    OfflineTtsModelConfig modelConfig =
        OfflineTtsModelConfig.builder()
            .setKokoro(kokoroModelConfig)
            .setNumThreads(2)
            .setDebug(true)
            .build();
    OfflineTts tts =
        new OfflineTts(OfflineTtsConfig.builder().setModel(modelConfig).build());

    GenerationConfig genConfig = new GenerationConfig();
    genConfig.setSid(0);
    genConfig.setSpeed(1.0f);
    genConfig.setSilenceScale(0.2f);
    long start = System.currentTimeMillis();
    GeneratedAudio audio = tts.generateFromPhonemes(input, genConfig, samples -> 1);
    float elapsed = (System.currentTimeMillis() - start) / 1000.0f;

    String filename = "tts-kokoro-en.wav";
    audio.save(filename);
    System.out.printf("-- elapsed: %.3f seconds%n", elapsed);
    System.out.printf("-- sample rate: %d%n", audio.getSampleRate());
    System.out.printf("-- samples: %d%n", audio.getSamples().length);
    for (SpanAlignment alignment : audio.getSpanAlignments()) {
      System.out.printf("-- %s -> %s: %.3f-%.3f%n", alignment.originalPhonemes,
          alignment.inferredPhonemes, alignment.startTs, alignment.endTs);
    }
    System.out.printf("-- Saved to %s%n", filename);
    tts.release();
  }
}
