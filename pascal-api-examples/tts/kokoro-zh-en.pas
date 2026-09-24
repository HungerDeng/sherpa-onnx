{ Copyright (c)  2025  Xiaomi Corporation }
program kokoro_en;
{
This file shows how to use precomputed phonemes with sherpa-onnx
with Kokoro TTS models (Chinese + English).

It generates speech from phonemes and saves it to a wave file.

If you want to play it while it is generating, please see
./kokoro-en-playback.pas
}

{$mode objfpc}
{$codepage utf8}

uses
  SysUtils,
  sherpa_onnx;

function GetOfflineTts: TSherpaOnnxOfflineTts;
var
  Config: TSherpaOnnxOfflineTtsConfig;
begin
  Config.Model.Kokoro.Model := './kokoro-multi-lang-v1_0/model.onnx';
  Config.Model.Kokoro.Voices := './kokoro-multi-lang-v1_0/voices.bin';
  Config.Model.Kokoro.Tokens := './kokoro-multi-lang-v1_0/tokens.txt';
  Config.Model.NumThreads := 2;
  Config.Model.Debug := False;
  Config.MaxNumSentences := 1;

  Result := TSherpaOnnxOfflineTts.Create(Config);
end;

var
  Tts: TSherpaOnnxOfflineTts;
  Audio: TSherpaOnnxGeneratedAudio;
  GenerationConfig: TSherpaOnnxGenerationConfig;

  Input: TSherpaOnnxPhonemeInput;
  Speed: Single = 1.0;  {Use a larger value to speak faster}
  SpeakerId: Integer = 46;

begin
  Tts := GetOfflineTts;

  WriteLn('There are ', Tts.GetNumSpeakers, ' speakers');

  Input.Phonemes := 'ðə pɹˈaɪs ɪz wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz. ɐ bˈɪt ɛkspˈɛnsɪv.';
  SetLength(Input.Spans, 9);
  Input.Spans[0].Phonemes := 'ðə';
  Input.Spans[1].Phonemes := 'pɹˈaɪs';
  Input.Spans[2].Phonemes := 'ɪz';
  Input.Spans[3].Phonemes := 'wˈʌn θˈaʊzənd tˈuː hˈʌndɹɪd dˈɑːlɚz';
  Input.Spans[4].Phonemes := '.';
  Input.Spans[5].Phonemes := 'ɐ';
  Input.Spans[6].Phonemes := 'bˈɪt';
  Input.Spans[7].Phonemes := 'ɛkspˈɛnsɪv';
  Input.Spans[8].Phonemes := '.';

  GenerationConfig := Default(TSherpaOnnxGenerationConfig);
  GenerationConfig.SilenceScale := 0.2;
  GenerationConfig.Speed := Speed;
  GenerationConfig.Sid := SpeakerId;

  Audio :=  Tts.GenerateFromPhonemes(Input, GenerationConfig, NIL, NIL);
  SherpaOnnxWriteWave('./kokoro-zh-en-46.wav', Audio.Samples, Audio.SampleRate);
  WriteLn('Saved to ./kokoro-zh-en-46.wav');

  FreeAndNil(Tts);
end.