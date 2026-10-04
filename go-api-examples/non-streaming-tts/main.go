package main

import (
	"encoding/json"
	"log"
	"math"
	"os"

	sherpa "github.com/k2-fsa/sherpa-onnx-go/sherpa_onnx"
	flag "github.com/spf13/pflag"
)

func main() {
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	config := sherpa.OfflineTtsConfig{}
	sid := 0
	filename := "./generated.wav"
	g2pOutputFile := ""

	var speed float32

	flag.StringVar(&config.Model.Vits.Model, "vits-model", "", "Path to the vits ONNX model")
	flag.StringVar(&config.Model.Vits.Lexicon, "vits-lexicon", "", "Path to lexicon.txt")
	flag.StringVar(&config.Model.Vits.Tokens, "vits-tokens", "", "Path to tokens.txt")
	flag.StringVar(&config.Model.Vits.DataDir, "vits-data-dir", "", "Path to espeak-ng-data")
	flag.Float32Var(&config.Model.Vits.NoiseScale, "vits-noise-scale", 0.667, "noise_scale for VITS")
	flag.Float32Var(&config.Model.Vits.NoiseScaleW, "vits-noise-scale-w", 0.8, "noise_scale_w for VITS")
	flag.Float32Var(&config.Model.Vits.LengthScale, "vits-length-scale", 1.0, "length_scale for VITS. small -> faster; large -> slower")

	flag.StringVar(&config.Model.Matcha.AcousticModel, "matcha-acoustic-model", "", "Path to the matcha acoustic model")
	flag.StringVar(&config.Model.Matcha.Vocoder, "matcha-vocoder", "", "Path to the matcha vocoder model")
	flag.StringVar(&config.Model.Matcha.Lexicon, "matcha-lexicon", "", "Path to lexicon.txt")
	flag.StringVar(&config.Model.Matcha.Tokens, "matcha-tokens", "", "Path to tokens.txt")
	flag.StringVar(&config.Model.Matcha.DataDir, "matcha-data-dir", "", "Path to espeak-ng-data")
	flag.Float32Var(&config.Model.Matcha.NoiseScale, "matcha-noise-scale", 0.667, "noise_scale for Matcha")
	flag.Float32Var(&config.Model.Matcha.LengthScale, "matcha-length-scale", 1.0, "length_scale for Matcha. small -> faster; large -> slower")

	flag.StringVar(&config.Model.Kokoro.Model, "kokoro-model", "", "Path to the Kokoro ONNX model")
	flag.StringVar(&config.Model.Kokoro.Voices, "kokoro-voices", "", "Path to voices.bin for Kokoro")
	flag.StringVar(&config.Model.Kokoro.Tokens, "kokoro-tokens", "", "Path to tokens.txt for Kokoro")
	flag.Float32Var(&config.Model.Kokoro.LengthScale, "kokoro-length-scale", 1.0, "length_scale for Kokoro. small -> faster; large -> slower")
	flag.StringVar(&g2pOutputFile, "g2p-output", "", "Path to misaki-rs G2pOutput JSON for Kokoro")

	flag.StringVar(&config.Model.Kitten.Model, "kitten-model", "", "Path to the kitten ONNX model")
	flag.StringVar(&config.Model.Kitten.Voices, "kitten-voices", "", "Path to voices.bin for kitten")
	flag.StringVar(&config.Model.Kitten.Tokens, "kitten-tokens", "", "Path to tokens.txt for kitten")
	flag.StringVar(&config.Model.Kitten.DataDir, "kitten-data-dir", "", "Path to espeak-ng-data for kitten")
	flag.Float32Var(&config.Model.Kitten.LengthScale, "kitten-length-scale", 1.0, "length_scale for kitten. small -> faster; large -> slower")

	flag.Float32Var(&speed, "speed", 1.0, "Speech speed. larger->faster; smaller->slower")

	flag.IntVar(&config.Model.NumThreads, "num-threads", 1, "Number of threads for computing")
	flag.IntVar(&config.Model.Debug, "debug", 0, "Whether to show debug message")
	flag.StringVar(&config.Model.Provider, "provider", "cpu", "Provider to use: cpu/cuda/coreml")
	flag.StringVar(&config.RuleFsts, "tts-rule-fsts", "", "Path to rule.fst")
	flag.StringVar(&config.RuleFars, "tts-rule-fars", "", "Path to rule.far")
	flag.IntVar(&config.MaxNumSentences, "tts-max-num-sentences", 1, "Batch size (split long text to avoid OOM)")

	flag.IntVar(&sid, "sid", sid, "Speaker ID (multi-speaker models only)")
	flag.StringVar(&filename, "output-filename", filename, "Output wav filename")

	flag.Parse()

	var text string
	var phonemeInput sherpa.PhonemeInput
	if config.Model.Kokoro.Model != "" {
		if g2pOutputFile == "" || len(flag.Args()) != 0 {
			log.Fatal("Kokoro requires --g2p-output and no text argument")
		}
		data, err := os.ReadFile(g2pOutputFile)
		if err != nil {
			log.Fatal(err)
		}
		if err := json.Unmarshal(data, &phonemeInput); err != nil {
			log.Fatal(err)
		}
		log.Println("Input phonemes:", phonemeInput.Phonemes)
	} else {
		if len(flag.Args()) != 1 {
			log.Fatal("Please provide the text to generate audio")
		}
		text = flag.Arg(0)
		log.Println("Input text:", text)
	}

	log.Println("Speaker ID:", sid)
	log.Println("Output filename:", filename)

	log.Println("Initializing model (may take several seconds)")
	tts := sherpa.NewOfflineTts(&config)
	defer sherpa.DeleteOfflineTts(tts)
	log.Println("Model created!")

	log.Println("Start generating!")
	cfg := sherpa.GenerationConfig{
		SilenceScale: 0.2,
		Speed:        float32(math.Max(float64(speed), 1e-6)),
		Sid:          sid,
	}
	var audio *sherpa.GeneratedAudio
	if config.Model.Kokoro.Model != "" {
		audio = tts.GenerateFromPhonemesWithConfig(phonemeInput, &cfg, nil)
	} else {
		audio = tts.GenerateWithConfig(text, &cfg, nil)
	}
	if audio == nil {
		log.Fatal("TTS generation failed")
	}

	log.Println("Done!")
	if config.Model.Kokoro.Model != "" {
		log.Printf("Aligned spans: %d", len(audio.SpanAlignments))
	}
	if ok := audio.Save(filename); !ok {
		log.Fatalf("Failed to write %s", filename)
	}
	log.Println("Saved to", filename)
}
