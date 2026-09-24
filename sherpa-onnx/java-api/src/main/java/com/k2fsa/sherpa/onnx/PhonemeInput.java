package com.k2fsa.sherpa.onnx;

public class PhonemeInput {
    public final String phonemes;
    public final PhonemeSpan[] spans;

    public PhonemeInput(String phonemes, PhonemeSpan[] spans) {
        this.phonemes = phonemes;
        this.spans = spans;
    }
}
