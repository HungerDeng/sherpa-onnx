//
//  ContentView.swift
//  SherpaOnnxTts
//
//  Created by fangjun on 2023/11/23.
//
// Text-to-speech with Next-gen Kaldi on iOS without Internet connection

#if canImport(SherpaOnnx)
import SherpaOnnx
#elseif canImport(SherpaOnnxShared)
import SherpaOnnxShared
#else
#error("SherpaOnnx module not found. Please check your SPM dependency configuration.")
#endif
import SwiftUI
import AVFoundation
import UniformTypeIdentifiers

private struct MisakiG2pOutput: Decodable {
    let phonemes: String
    let spans: [MisakiSpan]
}

private struct MisakiSpan: Decodable {
    let phonemes: String
}

class TtsProgressHandler: ObservableObject {
    @Published var progress: Float = 0.0
    @Published var isGenerating = false
    private var audioEngine: AVAudioEngine?
    private var playerNode: AVAudioPlayerNode?
    private var audioFormat: AVAudioFormat?
    private var sampleRate: Float = 22050
    private var pendingBuffers = 0
    private var shouldStop = false
    private let lock = NSLock()

    func startPlayback(sampleRate: Float) {
        self.sampleRate = sampleRate
        lock.lock()
        self.pendingBuffers = 0
        self.shouldStop = false
        lock.unlock()

        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default)
            try session.setActive(true)
        } catch {
            print("AVAudioSession error: \(error)")
        }

        let engine = AVAudioEngine()
        let player = AVAudioPlayerNode()
        let format = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: Double(sampleRate),
            channels: 1,
            interleaved: false)

        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: format)

        do {
            try engine.start()
        } catch {
            print("AVAudioEngine start error: \(error)")
            return
        }

        self.audioEngine = engine
        self.playerNode = player
        self.audioFormat = format

        player.play()

        DispatchQueue.main.async {
            self.isGenerating = true
            self.progress = 0.0
        }
    }

    func appendSamples(_ samples: UnsafePointer<Float>?, count: Int32, progress: Float) {
        lock.lock()
        let stop = shouldStop
        lock.unlock()
        guard !stop,
              let playerNode = playerNode,
              let audioFormat = audioFormat,
              let samples = samples,
              count > 0
        else { return }

        let frameCount = AVAudioFrameCount(count)
        guard let buffer = AVAudioPCMBuffer(
            pcmFormat: audioFormat, frameCapacity: frameCount)
        else { return }

        buffer.frameLength = frameCount
        let channelData = buffer.floatChannelData![0]
        memcpy(channelData, samples, Int(count) * MemoryLayout<Float>.size)

        lock.lock()
        pendingBuffers += 1
        lock.unlock()

        playerNode.scheduleBuffer(buffer) { [weak self] in
            guard let self = self else { return }
            self.lock.lock()
            self.pendingBuffers -= 1
            self.lock.unlock()
        }

        DispatchQueue.main.async {
            self.progress = progress
        }
    }

    /// Returns 0 to stop generation, 1 to continue
    var stopFlag: Int32 {
        lock.lock()
        defer { lock.unlock() }
        return shouldStop ? 0 : 1
    }

    func requestStop() {
        lock.lock()
        shouldStop = true
        lock.unlock()
    }

    func finishGeneration() {
        // Wait for all scheduled buffers to finish playing, then clean up
        DispatchQueue.global(qos: .background).async { [weak self] in
            guard let self = self else { return }
            while true {
                self.lock.lock()
                let pending = self.pendingBuffers
                let stop = self.shouldStop
                self.lock.unlock()
                if pending <= 0 || stop { break }
                Thread.sleep(forTimeInterval: 0.05)
            }

            self.lock.lock()
            let stopped = self.shouldStop
            self.lock.unlock()

            if stopped {
                self.playerNode?.stop()
            } else {
                Thread.sleep(forTimeInterval: 0.3)
            }

            DispatchQueue.main.async {
                self.playerNode?.stop()
                self.audioEngine?.stop()
                self.audioEngine = nil
                self.playerNode = nil
                self.audioFormat = nil
                self.isGenerating = false
                if !stopped {
                    self.progress = 1.0
                }
            }
        }
    }
}

struct ContentView: View {
    @State private var sid = "0"
    @State private var speed = 1.0
    @State private var text = ""
    @State private var showAlert = false
    @State private var alertMessage = ""
    @State private var audioInfo: String?
    @State private var spanAlignments: [SherpaOnnxSpanAlignmentSwift]?
    @State var filename: URL = NSURL() as URL
    @State var audioPlayer: AVAudioPlayer!

    @State private var lang = "en"
    @State private var numSteps = 5

    @StateObject private var progressHandler = TtsProgressHandler()

    @State private var showSavePicker = false

    private let languages = [
        "en", "ko", "ja", "ar", "bg", "cs", "da", "de", "el", "es",
        "et", "fi", "fr", "hi", "hr", "hu", "id", "it", "lt", "lv",
        "nl", "pl", "pt", "ro", "ru", "sk", "sl", "sv", "tr", "uk", "vi"
    ]

    private var tts = createOfflineTts()

    var body: some View {

        VStack(alignment: .leading) {
            HStack {
                Spacer()
                Text("Next-gen Kaldi: TTS").font(.title)
                Spacer()
            }
            if tts.numSpeakers > 1 {
                HStack {
                    Text("Speaker (1-\(tts.numSpeakers))")
                    Stepper("\((Int(sid) ?? 0) + 1)", value: Binding(
                        get: { (Int(sid) ?? 0) + 1 },
                        set: { sid = "\($0 - 1)" }
                    ), in: 1...Int(tts.numSpeakers))
                }
            }
            HStack{
                Text("Speed \(String(format: "%.1f", speed))")
                    .padding(.trailing)
                Slider(value: $speed, in: 0.5...2.0, step: 0.1) {
                    Text("Speech speed")
                }
            }

            if tts.isSupertonic {
                HStack {
                    Text("Language")
                    Picker("Language", selection: $lang) {
                        ForEach(languages, id: \.self) { l in
                            Text(l).tag(l)
                        }
                    }
                    .pickerStyle(.menu)

                    Spacer()

                    Text("Steps")
                    Stepper("\(numSteps)", value: $numSteps, in: 1...20)
                }
            }

            if progressHandler.isGenerating {
                VStack(spacing: 4) {
                    ProgressView(value: Double(progressHandler.progress))
                        .progressViewStyle(.linear)
                    Text(String(format: "%.0f%%", progressHandler.progress * 100))
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                .padding(.vertical, 4)
            }

            Text(selectedTtsExampleModel == .kokoroV1
                 ? "Paste the misaki-rs G2pOutput JSON below"
                 : "Please input your text below")
                .padding([.trailing, .top, .bottom])

            TextEditor(text: $text)
                .font(.body)
                .opacity(self.text.isEmpty ? 0.25 : 1)
                .disableAutocorrection(true)
                .border(Color.black)
                .frame(minHeight: 100)

            if let audioInfo = audioInfo {
                Text(audioInfo).font(.caption).foregroundColor(.secondary)
            }
            if let alignments = spanAlignments {
                Text("Span alignments").font(.headline)
                ScrollView {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(alignments.indices, id: \.self) { index in
                            let row = alignments[index]
                            Text("\(row.originalPhonemes) → \(row.inferredPhonemes) (\(row.startTs, specifier: "%.2f")–\(row.endTs, specifier: "%.2f") s)")
                                .font(.caption)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                }
                .frame(maxHeight: 150)
            }

            Spacer()
            HStack {
                Spacer()
                if progressHandler.isGenerating {
                    Button(action: {
                        progressHandler.requestStop()
                    }) {
                        Text("Stop")
                    }
                } else {
                    Button(action: {
                        generate()
                    }) {
                        Text("Generate")
                    }
                }
                Spacer()
                Button(action: {
                    self.audioPlayer.play()
                }) {
                    Text("Play")
                }.disabled(filename.absoluteString.isEmpty || progressHandler.isGenerating)
                Spacer()
                Button(action: {
                    showSavePicker = true
                }) {
                    Text("Save")
                }
                .disabled(filename.absoluteString.isEmpty || progressHandler.isGenerating)
                .fileExporter(
                    isPresented: $showSavePicker,
                    document: WavDocument(url: filename),
                    contentType: .wav,
                    defaultFilename: "sherpa-onnx-tts-output"
                ) { result in
                    // fileExporter handles the save
                }
                Spacer()
                Button(action: {
                    shareWav()
                }) {
                    Text("Share")
                }.disabled(filename.absoluteString.isEmpty || progressHandler.isGenerating)
                Spacer()
            }
            Spacer()
        }
        .padding()
        .alert(isPresented: $showAlert) {
            Alert(title: Text("Cannot generate audio"), message: Text(alertMessage))
        }
    }

    private func generate() {
        let speakerId = Int(self.sid) ?? 0
        let t = self.text.trimmingCharacters(in: .whitespacesAndNewlines)
        if t.isEmpty {
            self.alertMessage = selectedTtsExampleModel == .kokoroV1
                ? "Paste a misaki-rs G2pOutput JSON object first."
                : "Please input your text before clicking Generate."
            self.showAlert = true
            return
        }

        let phonemeInput: MisakiG2pOutput?
        if selectedTtsExampleModel == .kokoroV1 {
            do {
                let decoded = try JSONDecoder().decode(
                    MisakiG2pOutput.self, from: Data(t.utf8))
                guard !decoded.phonemes.isEmpty, !decoded.spans.isEmpty else {
                    alertMessage = "G2pOutput needs phonemes and at least one span."
                    showAlert = true
                    return
                }
                phonemeInput = decoded
            } catch {
                alertMessage = "Invalid G2pOutput JSON: \(error.localizedDescription)"
                showAlert = true
                return
            }
        } else {
            phonemeInput = nil
        }

        audioInfo = nil
        spanAlignments = nil

        if self.filename.absoluteString.isEmpty {
            let tempDirectoryURL = NSURL.fileURL(
                withPath: NSTemporaryDirectory(), isDirectory: true)
            self.filename = tempDirectoryURL.appendingPathComponent("test.wav")
        }

        let handler = progressHandler
        let sampleRate = Float(tts.sampleRate)
        let currentSpeed = Float(self.speed)
        let currentNumSteps = self.numSteps
        let currentLang = self.lang
        let currentFilename = self.filename
        let isSupertonic = tts.isSupertonic
        let isKokoro = selectedTtsExampleModel == .kokoroV1

        DispatchQueue.global(qos: .userInitiated).async {
            handler.startPlayback(sampleRate: sampleRate)

            let arg = Unmanaged.passUnretained(handler).toOpaque()

            let audio: SherpaOnnxGeneratedAudioWrapper

            if isKokoro || isSupertonic {
                let progressCallback: TtsProgressCallbackWithArg = {
                    samples, n, progress, arg in
                    let h = Unmanaged<TtsProgressHandler>.fromOpaque(arg!)
                        .takeUnretainedValue()
                    h.appendSamples(samples, count: n, progress: progress)
                    return h.stopFlag
                }

                var genConfig = SherpaOnnxGenerationConfigSwift()
                genConfig.sid = speakerId
                genConfig.speed = currentSpeed
                if let phonemeInput = phonemeInput {
                    audio = tts.generateFromPhonemes(
                        phonemes: phonemeInput.phonemes,
                        spans: phonemeInput.spans.map(\.phonemes),
                        config: genConfig,
                        callback: progressCallback, arg: arg)
                } else {
                    genConfig.numSteps = currentNumSteps
                    genConfig.extra = ["lang": currentLang]
                    audio = tts.generateWithConfig(
                        text: t, config: genConfig,
                        callback: progressCallback, arg: arg)
                }
            } else {
                let simpleCallback: TtsCallbackWithArg = { samples, n, arg in
                    let h = Unmanaged<TtsProgressHandler>.fromOpaque(arg!)
                        .takeUnretainedValue()
                    h.appendSamples(samples, count: n, progress: 1.0)
                    return h.stopFlag
                }

                audio = tts.generateWithCallbackWithArg(
                    text: t, callback: simpleCallback, arg: arg,
                    sid: speakerId, speed: currentSpeed)
            }

            let saved = audio.audio != nil && audio.save(filename: currentFilename.path) == 1
            let generatedSampleRate = audio.audio != nil ? audio.sampleRate : 0
            let generatedSampleCount = audio.audio != nil ? audio.n : 0
            let generatedAlignments = audio.audio != nil ? audio.spanAlignments : nil

            handler.finishGeneration()

            DispatchQueue.main.async {
                if saved {
                    self.audioInfo = "\(generatedSampleCount) samples at \(generatedSampleRate) Hz"
                    self.spanAlignments = generatedAlignments
                    self.audioPlayer = try? AVAudioPlayer(contentsOf: currentFilename)
                } else {
                    self.alertMessage = "Audio generation failed."
                    self.showAlert = true
                }
            }
        }
    }

    private func shareWav() {
        guard !filename.absoluteString.isEmpty else { return }
        let activityVC = UIActivityViewController(
            activityItems: [filename], applicationActivities: nil)
        guard let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene,
              let rootVC = scene.windows.first?.rootViewController
        else { return }
        rootVC.present(activityVC, animated: true)
    }
}

struct WavDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.wav] }

    let url: URL

    init(url: URL) {
        self.url = url
    }

    init(configuration: ReadConfiguration) throws {
        fatalError("init(configuration:) not implemented")
    }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        let data = try Data(contentsOf: url)
        return FileWrapper(regularFileWithContents: data)
    }
}

struct ContentView_Previews: PreviewProvider {
    static var previews: some View {
        ContentView()
    }
}
