// Copyright 2024 Xiaomi Corporation

package com.k2fsa.sherpa.onnx;

public class GeneratedAudio {
    private final float[] samples;
    private final int sampleRate;
    private final SpanAlignment[] spanAlignments;

    public GeneratedAudio(float[] samples, int sampleRate) {
        this(samples, sampleRate, null);
    }

    public GeneratedAudio(float[] samples, int sampleRate, SpanAlignment[] spanAlignments) {
        LibraryLoader.maybeLoad();
        this.samples = samples;
        this.sampleRate = sampleRate;
        this.spanAlignments = spanAlignments;
    }

    public int getSampleRate() {
        return sampleRate;
    }

    public float[] getSamples() {
        return samples;
    }

    public SpanAlignment[] getSpanAlignments() {
        return spanAlignments;
    }

    // return true if saved successfully.
    public boolean save(String filename) {
        return saveImpl(filename, samples, sampleRate);
    }

    private native boolean saveImpl(String filename, float[] samples, int sampleRate);
}
