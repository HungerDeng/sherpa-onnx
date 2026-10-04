// Kokoro consumes phonemes from misaki-rs and returns audio plus span timings.
using System;
using SherpaOnnx;

class KokoroTtsDemo
{
    static void Main()
    {
        var model = "./kokoro-multi-lang-v1_0";
        var config = new OfflineTtsConfig();
        config.Model.Kokoro.Model = model + "/model.onnx";
        config.Model.Kokoro.Voices = model + "/voices.bin";
        config.Model.Kokoro.Tokens = model + "/tokens.txt";
        config.Model.NumThreads = 2;

        // Only these fields from a misaki-rs G2pOutput are passed to Kokoro.
        var input = new PhonemeInput {
            Phonemes = "ðə pɹˈaɪs ɪz wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz. ɐ bˈɪt ɛkspˈɛnsɪv.",
            Spans = new[] {
                new PhonemeSpan { Phonemes = "ðə" },
                new PhonemeSpan { Phonemes = "pɹˈaɪs" },
                new PhonemeSpan { Phonemes = "ɪz" },
                new PhonemeSpan { Phonemes = "wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz" },
                new PhonemeSpan { Phonemes = "." },
                new PhonemeSpan { Phonemes = "ɐ" },
                new PhonemeSpan { Phonemes = "bˈɪt" },
                new PhonemeSpan { Phonemes = "ɛkspˈɛnsɪv" },
                new PhonemeSpan { Phonemes = "." },
            },
        };
        using (var tts = new OfflineTts(config))
        {
            var generation = new OfflineTtsGenerationConfig();
            generation.Sid = 20;
            var audio = tts.GenerateFromPhonemesWithConfig(input, generation, null);
            if (audio == null) throw new Exception("Kokoro generation failed");
            Console.WriteLine($"{audio.NumSamples} samples at {audio.SampleRate} Hz");
            foreach (var alignment in audio.SpanAlignments)
                Console.WriteLine($"{alignment.OriginalPhonemes} -> {alignment.InferredPhonemes}: {alignment.StartTs:F3}-{alignment.EndTs:F3}s");
            audio.SaveToWaveFile("./generated-kokoro.wav");
            audio.Dispose();
        }
    }
}
