// Copyright 2026 Xiaomi Corporation

package com.k2fsa.sherpa.onnx;

public class TermAlignment {
    private final String text;
    // G2P output before model-token filtering and alias canonicalization.
    private final String rawPhonemes;
    // Canonical phonemes reconstructed from the model token IDs.
    private final String inferredPhonemes;
    private final float startTs;
    private final float endTs;

    public TermAlignment(String text, String rawPhonemes, String inferredPhonemes,
                         float startTs, float endTs) {
        this.text = text;
        this.rawPhonemes = rawPhonemes;
        this.inferredPhonemes = inferredPhonemes;
        this.startTs = startTs;
        this.endTs = endTs;
    }

    public String getText() { return text; }
    public String getRawPhonemes() { return rawPhonemes; }
    public String getInferredPhonemes() { return inferredPhonemes; }
    public float getStartTs() { return startTs; }
    public float getEndTs() { return endTs; }
}
