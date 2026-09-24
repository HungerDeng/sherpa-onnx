package com.k2fsa.sherpa.onnx;

public class SpanAlignment {
    public final String originalPhonemes;
    public final String inferredPhonemes;
    public final float startTs;
    public final float endTs;

    public SpanAlignment(String originalPhonemes, String inferredPhonemes,
                         float startTs, float endTs) {
        this.originalPhonemes = originalPhonemes;
        this.inferredPhonemes = inferredPhonemes;
        this.startTs = startTs;
        this.endTs = endTs;
    }
}
