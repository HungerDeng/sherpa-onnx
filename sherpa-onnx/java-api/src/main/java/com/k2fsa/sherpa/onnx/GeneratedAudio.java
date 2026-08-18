// Copyright 2024 Xiaomi Corporation

package com.k2fsa.sherpa.onnx;

public class GeneratedAudio {
    private final float[] samples;
    private final int sampleRate;
    private final TermAlignment[] termAlignments;

    public GeneratedAudio(float[] samples, int sampleRate) {
        this(samples, sampleRate, null);
    }

    public GeneratedAudio(float[] samples, int sampleRate, TermAlignment[] termAlignments) {
        LibraryLoader.maybeLoad();
        this.samples = samples;
        this.sampleRate = sampleRate;
        this.termAlignments = termAlignments;
    }

    public int getSampleRate() {
        return sampleRate;
    }

    public float[] getSamples() {
        return samples;
    }

    /** Returns Kokoro v1.0+ frontend terms, or null when unavailable. */
    public TermAlignment[] getTermAlignments() {
        return termAlignments;
    }

    // return true if saved successfully.
    public boolean save(String filename) {
        return saveImpl(filename, samples, sampleRate);
    }

    private native boolean saveImpl(String filename, float[] samples, int sampleRate);
}
