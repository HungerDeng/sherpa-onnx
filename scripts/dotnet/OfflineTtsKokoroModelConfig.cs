/// Copyright (c)  2025  Xiaomi Corporation (authors: Fangjun Kuang)

using System.Runtime.InteropServices;

namespace SherpaOnnx
{
    [StructLayout(LayoutKind.Sequential)]
    public struct OfflineTtsKokoroModelConfig
    {
        public OfflineTtsKokoroModelConfig()
        {
            Model = "";
            Voices = "";
            Tokens = "";
            LengthScale = 1.0F;
        }
        [MarshalAs(UnmanagedType.LPStr)]
        public string Model;

        [MarshalAs(UnmanagedType.LPStr)]
        public string Voices;

        [MarshalAs(UnmanagedType.LPStr)]
        public string Tokens;

        public float LengthScale;
    }
}
