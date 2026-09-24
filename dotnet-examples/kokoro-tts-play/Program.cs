// Copyright (c)  2025  Xiaomi Corporation
//
// This file shows how to use a non-streaming Kokoro TTS model
// for text-to-speech
// Please refer to
// https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/index.html
// and
// https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models
// to download pre-trained models
using PortAudioSharp;
using SherpaOnnx;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;

class KokoroTtsPlayDemo
{
  static void Main(string[] args)
  {
    var config = new OfflineTtsConfig();
    config.Model.Kokoro.Model = "./kokoro-multi-lang-v1_0/model.onnx";
    config.Model.Kokoro.Voices = "./kokoro-multi-lang-v1_0/voices.bin";
    config.Model.Kokoro.Tokens = "./kokoro-multi-lang-v1_0/tokens.txt";

    config.Model.NumThreads = 2;
    config.Model.Debug = 1;
    config.Model.Provider = "cpu";

    var tts = new OfflineTts(config);
    var speed = 1.0f;
    // A G2pOutput from misaki-rs, reduced to phonemes and spans[].phonemes.
    var input = new PhonemeInput {
      Phonemes = "ˌeɪˈaɪ ɪz sˌoʊ ˈɔːsʌm. aɪ kˈænt lˈɪv wɪðˈaʊt ɪt.",
      Spans = new[] {
        new PhonemeSpan { Phonemes = "ˌeɪˈaɪ" },
        new PhonemeSpan { Phonemes = "ɪz" },
        new PhonemeSpan { Phonemes = "sˌoʊ" },
        new PhonemeSpan { Phonemes = "ˈɔːsʌm" },
        new PhonemeSpan { Phonemes = "." },
        new PhonemeSpan { Phonemes = "aɪ" },
        new PhonemeSpan { Phonemes = "kˈænt" },
        new PhonemeSpan { Phonemes = "lˈɪv" },
        new PhonemeSpan { Phonemes = "wɪðˈaʊt" },
        new PhonemeSpan { Phonemes = "ɪt" },
        new PhonemeSpan { Phonemes = "." },
      },
    };

    var sid = 0;
    OfflineTtsGenerationConfig genConfig = new OfflineTtsGenerationConfig();
    genConfig.Sid = sid;
    genConfig.Speed = speed;
    genConfig.SilenceScale = 0.2f;


    Console.WriteLine(PortAudio.VersionInfo.versionText);
    PortAudio.Initialize();
    Console.WriteLine($"Number of devices: {PortAudio.DeviceCount}");

    for (int i = 0; i != PortAudio.DeviceCount; ++i)
    {
      Console.WriteLine($" Device {i}");
      DeviceInfo deviceInfo = PortAudio.GetDeviceInfo(i);
      Console.WriteLine($"   Name: {deviceInfo.name}");
      Console.WriteLine($"   Max output channels: {deviceInfo.maxOutputChannels}");
      Console.WriteLine($"   Default sample rate: {deviceInfo.defaultSampleRate}");
    }
    int deviceIndex = PortAudio.DefaultOutputDevice;
    if (deviceIndex == PortAudio.NoDevice)
    {
      Console.WriteLine("No default output device found. Please use ../offline-tts instead");
      Environment.Exit(1);
    }

    var info = PortAudio.GetDeviceInfo(deviceIndex);
    Console.WriteLine();
    Console.WriteLine($"Use output default device {deviceIndex} ({info.name})");

    var param = new StreamParameters();
    param.device = deviceIndex;
    param.channelCount = 1;
    param.sampleFormat = SampleFormat.Float32;
    param.suggestedLatency = info.defaultLowOutputLatency;
    param.hostApiSpecificStreamInfo = IntPtr.Zero;

    // https://learn.microsoft.com/en-us/dotnet/standard/collections/thread-safe/blockingcollection-overview
    var dataItems = new BlockingCollection<float[]>();

    var MyCallback = (IntPtr samples, int n, float progress, IntPtr arg) =>
    {
      Console.WriteLine($"Progress {progress*100}%");

      float[] data = new float[n];

      Marshal.Copy(samples, data, 0, n);

      dataItems.Add(data);

      // 1 means to keep generating
      // 0 means to stop generating
      return 1;
    };

    var playFinished = false;

    float[]? lastSampleArray = null;
    int lastIndex = 0; // not played

    PortAudioSharp.Stream.Callback playCallback = (IntPtr input, IntPtr output,
        UInt32 frameCount,
        ref StreamCallbackTimeInfo timeInfo,
        StreamCallbackFlags statusFlags,
        IntPtr userData
        ) =>
    {
      if (dataItems.IsCompleted && lastSampleArray == null && lastIndex == 0)
      {
        Console.WriteLine($"Finished playing");
        playFinished = true;
        return StreamCallbackResult.Complete;
      }

      int expected = Convert.ToInt32(frameCount);
      int i = 0;

      while ((lastSampleArray != null || dataItems.Count != 0) && (i < expected))
      {
        int needed = expected - i;

        if (lastSampleArray != null)
        {
          int remaining = lastSampleArray.Length - lastIndex;
          if (remaining >= needed)
          {
            float[] this_block = lastSampleArray.Skip(lastIndex).Take(needed).ToArray();
            lastIndex += needed;
            if (lastIndex == lastSampleArray.Length)
            {
              lastSampleArray = null;
              lastIndex = 0;
            }

            Marshal.Copy(this_block, 0, IntPtr.Add(output, i * sizeof(float)), needed);
            return StreamCallbackResult.Continue;
          }

          float[] this_block2 = lastSampleArray.Skip(lastIndex).Take(remaining).ToArray();
          lastIndex = 0;
          lastSampleArray = null;

          Marshal.Copy(this_block2, 0, IntPtr.Add(output, i * sizeof(float)), remaining);
          i += remaining;
          continue;
        }

        if (dataItems.Count != 0)
        {
          lastSampleArray = dataItems.Take();
          lastIndex = 0;
        }
      }

      if (i < expected)
      {
        int sizeInBytes = (expected - i) * 4;
        Marshal.Copy(new byte[sizeInBytes], 0, IntPtr.Add(output, i * sizeof(float)), sizeInBytes);
      }

      return StreamCallbackResult.Continue;
    };

    PortAudioSharp.Stream stream = new PortAudioSharp.Stream(inParams: null, outParams: param, sampleRate: tts.SampleRate,
        framesPerBuffer: 0,
        streamFlags: StreamFlags.ClipOff,
        callback: playCallback,
        userData: IntPtr.Zero
        );

    stream.Start();

    var callback = new OfflineTtsCallbackProgressWithArg(MyCallback);

    var audio = tts.GenerateFromPhonemesWithConfig(input, genConfig, callback);
    if (audio == null) throw new Exception("Kokoro generation failed");
    var outputFilename = "./generated-kokoro-0.wav";
    var ok = audio.SaveToWaveFile(outputFilename);

    if (ok)
    {
      Console.WriteLine($"Wrote to {outputFilename} succeeded!");
    }
    else
    {
      Console.WriteLine($"Failed to write {outputFilename}");
    }
    dataItems.CompleteAdding();

    while (!playFinished)
    {
      Thread.Sleep(100); // 100ms
    }
  }
}
