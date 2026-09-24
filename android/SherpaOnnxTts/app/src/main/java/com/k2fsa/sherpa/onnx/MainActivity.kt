package com.k2fsa.sherpa.onnx

import android.content.Intent
import android.content.res.AssetManager
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.MediaPlayer
import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.widget.Button
import android.widget.EditText
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.FileProvider
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

const val TAG = "sherpa-onnx"

class MainActivity : AppCompatActivity() {
    private lateinit var tts: OfflineTts
    private lateinit var text: EditText
    private lateinit var sid: EditText
    private lateinit var speed: EditText
    private lateinit var generate: Button
    private lateinit var play: Button
    private lateinit var stop: Button
    private lateinit var save: Button
    private lateinit var share: Button
    private var stopped: Boolean = false
    private var mediaPlayer: MediaPlayer? = null
    private var isSupertonic: Boolean = false
    private var supertonicLang: String = "en"

    private val saveLauncher = registerForActivityResult(ActivityResultContracts.CreateDocument("audio/wav")) { uri ->
        if (uri != null) {
            copyGeneratedWavToUri(uri)
        }
    }

    // see
    // https://developer.android.com/reference/kotlin/android/media/AudioTrack
    private lateinit var track: AudioTrack

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        Log.i(TAG, "Start to initialize TTS")
        initTts()
        Log.i(TAG, "Finish initializing TTS")

        Log.i(TAG, "Start to initialize AudioTrack")
        initAudioTrack()
        Log.i(TAG, "Finish initializing AudioTrack")

        text = findViewById(R.id.text)
        sid = findViewById(R.id.sid)
        speed = findViewById(R.id.speed)

        generate = findViewById(R.id.generate)
        play = findViewById(R.id.play)
        stop = findViewById(R.id.stop)
        save = findViewById(R.id.save)
        share = findViewById(R.id.share)

        generate.setOnClickListener { onClickGenerate() }
        play.setOnClickListener { onClickPlay() }
        stop.setOnClickListener { onClickStop() }
        save.setOnClickListener { onClickSave() }
        share.setOnClickListener { onClickShare() }

        sid.setText("0")
        speed.setText("1.0")

        // we will change sampleText here in the CI
        val sampleText = ""
        text.setText(sampleText)
        if (tts.config.model.kokoro.model.isNotEmpty()) {
            text.hint = "Paste a Misaki G2pOutput JSON object"
        }

        play.isEnabled = false
        save.isEnabled = false
        share.isEnabled = false
    }

    private fun initAudioTrack() {
        val sampleRate = tts.sampleRate()
        val bufLength = AudioTrack.getMinBufferSize(
            sampleRate,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_FLOAT
        )
        Log.i(TAG, "sampleRate: $sampleRate, buffLength: $bufLength")

        val attr = AudioAttributes.Builder().setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .build()

        val format = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
            .setSampleRate(sampleRate)
            .build()

        track = AudioTrack(
            attr, format, bufLength, AudioTrack.MODE_STREAM,
            AudioManager.AUDIO_SESSION_ID_GENERATE
        )
        track.play()
    }

    // this function is called from C++
    private fun callback(samples: FloatArray): Int {
        if (!stopped) {
            track.write(samples, 0, samples.size, AudioTrack.WRITE_BLOCKING)
            return 1
        } else {
            track.stop()
            return 0
        }
    }

    private fun onClickGenerate() {
        val sidInt = sid.text.toString().toIntOrNull()
        if (sidInt == null || sidInt < 0) {
            Toast.makeText(
                applicationContext,
                "Please input a non-negative integer for speaker ID!",
                Toast.LENGTH_SHORT
            ).show()
            return
        }

        val speedFloat = speed.text.toString().toFloatOrNull()
        if (speedFloat == null || speedFloat <= 0) {
            Toast.makeText(
                applicationContext,
                "Please input a positive number for speech speed!",
                Toast.LENGTH_SHORT
            ).show()
            return
        }

        val textStr = text.text.toString().trim()
        if (textStr.isBlank()) {
            Toast.makeText(applicationContext, "Please enter text or Misaki G2pOutput JSON", Toast.LENGTH_SHORT)
                .show()
            return
        }

        val phonemeInput = if (tts.config.model.kokoro.model.isNotEmpty()) {
            try {
                val output = JSONObject(textStr)
                val spans = output.getJSONArray("spans")
                PhonemeInput(
                    phonemes = output.getString("phonemes"),
                    spans = Array(spans.length()) { index ->
                        PhonemeSpan(spans.getJSONObject(index).getString("phonemes"))
                    },
                )
            } catch (e: Exception) {
                Toast.makeText(applicationContext, "Invalid Misaki G2pOutput JSON: ${e.message}", Toast.LENGTH_LONG).show()
                return
            }
        } else {
            null
        }

        track.pause()
        track.flush()
        track.play()

        play.isEnabled = false
        save.isEnabled = false
        share.isEnabled = false
        generate.isEnabled = false
        stopped = false
        Thread {
            try {
                val genConfig = GenerationConfig(sid = sidInt, speed = speedFloat)
                if (isSupertonic) {
                    genConfig.extra = mapOf("lang" to supertonicLang)
                }
                val audio = if (phonemeInput != null) {
                    tts.generateFromPhonemes(
                        input = phonemeInput,
                        config = genConfig,
                        callback = this::callback,
                    )
                } else {
                    tts.generateWithConfigAndCallback(
                        text = textStr,
                        config = genConfig,
                        callback = this::callback,
                    )
                }
                audio.spanAlignments?.forEach { alignment ->
                    Log.i(TAG, "${alignment.originalPhonemes} -> ${alignment.inferredPhonemes}: ${alignment.startTs}-${alignment.endTs}s")
                }

                val filename = application.filesDir.absolutePath + "/generated.wav"
                val ok = audio.samples.isNotEmpty() && audio.save(filename)
                runOnUiThread {
                    play.isEnabled = ok
                    save.isEnabled = ok
                    share.isEnabled = ok
                    generate.isEnabled = true
                    track.stop()
                }
            } catch (e: Exception) {
                Log.e(TAG, "TTS generation failed", e)
                runOnUiThread {
                    generate.isEnabled = true
                    track.stop()
                    Toast.makeText(applicationContext, "TTS generation failed: ${e.message}", Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    private fun onClickPlay() {
        val filename = application.filesDir.absolutePath + "/generated.wav"
        mediaPlayer?.stop()
        mediaPlayer = MediaPlayer.create(
            applicationContext,
            Uri.fromFile(File(filename))
        )
        mediaPlayer?.start()
    }

    private fun onClickStop() {
        stopped = true
        play.isEnabled = true
        save.isEnabled = true
        share.isEnabled = true
        generate.isEnabled = true
        track.pause()
        track.flush()
        mediaPlayer?.stop()
        mediaPlayer = null
    }

    private fun onClickSave() {
        saveLauncher.launch("generated.wav")
    }

    private fun onClickShare() {
        val file = File(application.filesDir.absolutePath + "/generated.wav")
        if (!file.exists()) {
            Toast.makeText(applicationContext, "No audio to share", Toast.LENGTH_SHORT).show()
            return
        }
        val uri = FileProvider.getUriForFile(
            this,
            "com.k2fsa.sherpa.onnx.fileprovider",
            file
        )
        val intent = Intent(Intent.ACTION_SEND).apply {
            type = "audio/wav"
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivity(Intent.createChooser(intent, "Share audio"))
    }

    private fun copyGeneratedWavToUri(destUri: Uri) {
        try {
            val srcFile = File(application.filesDir.absolutePath + "/generated.wav")
            contentResolver.openOutputStream(destUri)?.use { output ->
                srcFile.inputStream().use { input ->
                    input.copyTo(output)
                }
            }
            Toast.makeText(applicationContext, "Audio saved", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to save audio: $e")
            Toast.makeText(applicationContext, "Failed to save audio", Toast.LENGTH_SHORT).show()
        }
    }

    private fun initTts() {
        var modelDir: String?
        var modelName: String?
        var acousticModelName: String?
        var vocoder: String?
        var voices: String?
        var ruleFsts: String?
        var ruleFars: String?
        var lexicon: String?
        var dataDir: String?
        var assets: AssetManager? = application.assets
        var isKitten = false
        var isSupertonic = false
        var durationPredictor: String? = null
        var textEncoder: String? = null
        var vectorEstimator: String? = null
        var supertonicVocoder: String? = null
        var ttsJson: String? = null
        var unicodeIndexer: String? = null
        var voiceStyle: String? = null
        var supertonicLang: String = "en"

        // The purpose of such a design is to make the CI test easier
        // Please see
        // https://github.com/k2-fsa/sherpa-onnx/blob/master/scripts/apk/generate-tts-apk-script.py

        // VITS -- begin
        modelName = null
        // VITS -- end

        // Matcha -- begin
        acousticModelName = null
        vocoder = null
        // Matcha -- end

        // For Kokoro -- begin
        voices = null
        // For Kokoro -- end


        modelDir = null
        ruleFsts = null
        ruleFars = null
        lexicon = null
        dataDir = null

        // Example 1:
        // modelDir = "vits-vctk"
        // modelName = "vits-vctk.onnx"
        // lexicon = "lexicon.txt"

        // Example 2:
        // https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models
        // https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-piper-en_US-amy-low.tar.bz2
        // modelDir = "vits-piper-en_US-amy-low"
        // modelName = "en_US-amy-low.onnx"
        // dataDir = "vits-piper-en_US-amy-low/espeak-ng-data"

        // Example 3:
        // https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-icefall-zh-aishell3.tar.bz2
        // modelDir = "vits-icefall-zh-aishell3"
        // modelName = "model.onnx"
        // ruleFars = "vits-icefall-zh-aishell3/rule.far"
        // lexicon = "lexicon.txt"

        // Example 4:
        // https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/vits.html#csukuangfj-vits-zh-hf-fanchen-c-chinese-187-speakers
        // modelDir = "vits-zh-hf-fanchen-C"
        // modelName = "vits-zh-hf-fanchen-C.onnx"
        // lexicon = "lexicon.txt"

        // Example 5:
        // https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-coqui-de-css10.tar.bz2
        // modelDir = "vits-coqui-de-css10"
        // modelName = "model.onnx"

        // Example 6
        // vits-melo-tts-zh_en
        // https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/vits.html#vits-melo-tts-zh-en-chinese-english-1-speaker
        // modelDir = "vits-melo-tts-zh_en"
        // modelName = "model.onnx"
        // lexicon = "lexicon.txt"

        // Example 7
        // matcha-icefall-zh-baker
        // https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/matcha.html#matcha-icefall-zh-baker-chinese-1-female-speaker
        // modelDir = "matcha-icefall-zh-baker"
        // acousticModelName = "model-steps-3.onnx"
        // vocoder = "vocos-22khz-univ.onnx"    // Vocoder should be downloaded separately; place in the **root directory of your resources folder**, not under modelDir.
        // lexicon = "lexicon.txt"

        // Example 8
        // matcha-icefall-en_US-ljspeech
        // https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/matcha.html#matcha-icefall-en-us-ljspeech-american-english-1-female-speaker
        // modelDir = "matcha-icefall-en_US-ljspeech"
        // acousticModelName = "model-steps-3.onnx"
        // vocoder = "vocos-22khz-univ.onnx"
        // dataDir = "matcha-icefall-en_US-ljspeech/espeak-ng-data"

        // Example 9
        // kokoro-multi-lang-v1_0
        // Re-export model.onnx with pred_dur before using the phoneme-only API.
        // modelDir = "kokoro-multi-lang-v1_0"
        // modelName = "model.onnx"
        // voices = "voices.bin"

        // Example 10
        // kitten-nano-en-v0_1-fp16
        // modelDir = "kitten-nano-en-v0_1-fp16"
        // modelName = "model.fp16.onnx"
        // voices = "voices.bin"
        // dataDir = "kitten-nano-en-v0_1-fp16/espeak-ng-data"
        // isKitten = true

        // Example 11
        // matcha-icefall-zh-en
        // https://k2-fsa.github.io/sherpa/onnx/tts/all/Chinese-English/matcha-icefall-zh-en.html
        // modelDir = "matcha-icefall-zh-en"
        // acousticModelName = "model-steps-3.onnx"
        // vocoder = "vocos-16khz-univ.onnx"    // Vocoder should be downloaded separately; place in the **root directory of your resources folder**, not under modelDir.
        // dataDir = "matcha-icefall-zh-en/espeak-ng-data"
        // lexicon = "lexicon.txt"

        // Example 12
        // supertonic-3-tts (supports 31 languages, default: English)
        // https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models
        // modelDir = "sherpa-onnx-supertonic-3-tts-int8-2026-05-11"
        // isSupertonic = true
        // durationPredictor = "duration_predictor.int8.onnx"
        // textEncoder = "text_encoder.int8.onnx"
        // vectorEstimator = "vector_estimator.int8.onnx"
        // supertonicVocoder = "vocoder.int8.onnx"
        // ttsJson = "tts.json"
        // unicodeIndexer = "unicode_indexer.bin"
        // voiceStyle = "voice.bin"
        // supertonicLang = "en"  // ISO 639-1: en, zh, ja, ko, fr, de, es, etc.

        this.isSupertonic = isSupertonic
        this.supertonicLang = supertonicLang

        if (dataDir != null) {
            val newDir = copyDataDir(dataDir!!)
            dataDir = "$newDir/$dataDir"
        }

        val config = getOfflineTtsConfig(
            modelDir = modelDir!!,
            modelName = modelName ?: "",
            acousticModelName = acousticModelName ?: "",
            vocoder = vocoder ?: "",
            voices = voices ?: "",
            lexicon = lexicon ?: "",
            dataDir = dataDir ?: "",
            dictDir = "",
            ruleFsts = ruleFsts ?: "",
            ruleFars = ruleFars ?: "",
            isKitten = isKitten,
            isSupertonic = isSupertonic,
            durationPredictor = durationPredictor ?: "",
            textEncoder = textEncoder ?: "",
            vectorEstimator = vectorEstimator ?: "",
            supertonicVocoder = supertonicVocoder ?: "",
            ttsJson = ttsJson ?: "",
            unicodeIndexer = unicodeIndexer ?: "",
            voiceStyle = voiceStyle ?: "",
        )!!

        tts = OfflineTts(assetManager = assets, config = config)
    }


    private fun copyDataDir(dataDir: String): String {
        Log.i(TAG, "data dir is $dataDir")
        copyAssets(dataDir)

        val newDataDir = application.getExternalFilesDir(null)!!.absolutePath
        Log.i(TAG, "newDataDir: $newDataDir")
        return newDataDir
    }

    private fun copyAssets(path: String) {
        val assets: Array<String>?
        try {
            assets = application.assets.list(path)
            if (assets!!.isEmpty()) {
                copyFile(path)
            } else {
                val fullPath = "${application.getExternalFilesDir(null)}/$path"
                val dir = File(fullPath)
                dir.mkdirs()
                for (asset in assets.iterator()) {
                    val p: String = if (path == "") "" else path + "/"
                    copyAssets(p + asset)
                }
            }
        } catch (ex: IOException) {
            Log.e(TAG, "Failed to copy $path. $ex")
        }
    }

    private fun copyFile(filename: String) {
        try {
            val istream = application.assets.open(filename)
            val newFilename = application.getExternalFilesDir(null).toString() + "/" + filename
            val ostream = FileOutputStream(newFilename)
            // Log.i(TAG, "Copying $filename to $newFilename")
            val buffer = ByteArray(1024)
            var read = 0
            while (read != -1) {
                ostream.write(buffer, 0, read)
                read = istream.read(buffer)
            }
            istream.close()
            ostream.flush()
            ostream.close()
        } catch (ex: Exception) {
            Log.e(TAG, "Failed to copy $filename, $ex")
        }
    }
}
