// Copyright 2026 Xiaomi Corporation

package com.k2fsa.sherpa.onnx;

public class TermAlignment {
    private final String text;
    private final String phoneme;
    private final float startTs;
    private final float endTs;

    public TermAlignment(String text, String phoneme, float startTs, float endTs) {
        this.text = text;
        this.phoneme = phoneme;
        this.startTs = startTs;
        this.endTs = endTs;
    }

    public String getText() { return text; }
    public String getPhoneme() { return phoneme; }
    public float getStartTs() { return startTs; }
    public float getEndTs() { return endTs; }
}
