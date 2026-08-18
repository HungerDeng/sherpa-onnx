/// Copyright (c)  2024.5 by 东风破
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace SherpaOnnx
{
    public class OfflineTtsGeneratedAudio
    {
        public OfflineTtsGeneratedAudio(IntPtr p)
        {
            _handle = new HandleRef(this, p);
        }

        public bool SaveToWaveFile(String filename)
        {
            Impl impl = (Impl)Marshal.PtrToStructure(Handle, typeof(Impl));
            byte[] utf8Filename = Encoding.UTF8.GetBytes(filename);
            byte[] utf8FilenameWithNull = new byte[utf8Filename.Length + 1]; // +1 for null terminator
            Array.Copy(utf8Filename, utf8FilenameWithNull, utf8Filename.Length);
            utf8FilenameWithNull[utf8Filename.Length] = 0; // Null terminator
            int status = SherpaOnnxWriteWave(impl.Samples, impl.NumSamples, impl.SampleRate, utf8FilenameWithNull);
            return status == 1;
        }

        ~OfflineTtsGeneratedAudio()
        {
            Cleanup();
        }

        public void Dispose()
        {
            Cleanup();
            // Prevent the object from being placed on the
            // finalization queue
            System.GC.SuppressFinalize(this);
        }

        private void Cleanup()
        {
            SherpaOnnxDestroyOfflineTtsGeneratedAudio(Handle);

            // Don't permit the handle to be used again.
            _handle = new HandleRef(this, IntPtr.Zero);
        }

        [StructLayout(LayoutKind.Sequential)]
        struct Impl
        {
            public IntPtr Samples;
            public int NumSamples;
            public int SampleRate;
            public IntPtr TermAlignments;
            public int NumTermAlignments;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct AlignmentImpl
        {
            public IntPtr Text;
            public IntPtr Phoneme;
            public float StartTs;
            public float EndTs;
        }

        private HandleRef _handle;
        public IntPtr Handle => _handle.Handle;

        public int NumSamples
        {
            get
            {
                Impl impl = (Impl)Marshal.PtrToStructure(Handle, typeof(Impl));
                return impl.NumSamples;
            }
        }

        public int SampleRate
        {
            get
            {
                Impl impl = (Impl)Marshal.PtrToStructure(Handle, typeof(Impl));
                return impl.SampleRate;
            }
        }

        public float[] Samples
        {
            get
            {
                Impl impl = (Impl)Marshal.PtrToStructure(Handle, typeof(Impl));

                float[] samples = new float[impl.NumSamples];
                Marshal.Copy(impl.Samples, samples, 0, impl.NumSamples);
                return samples;
            }
        }

        public TermAlignment[] TermAlignments
        {
            get
            {
                Impl impl = (Impl)Marshal.PtrToStructure(Handle, typeof(Impl));
                if (impl.TermAlignments == IntPtr.Zero)
                    return null;

                TermAlignment[] ans = new TermAlignment[impl.NumTermAlignments];
                int size = Marshal.SizeOf(typeof(AlignmentImpl));
                for (int i = 0; i != ans.Length; ++i)
                {
                    IntPtr p = IntPtr.Add(impl.TermAlignments, i * size);
                    AlignmentImpl a = (AlignmentImpl)Marshal.PtrToStructure(p, typeof(AlignmentImpl));
                    ans[i] = new TermAlignment(
                        Utf8ToString(a.Text), Utf8ToString(a.Phoneme),
                        a.StartTs, a.EndTs);
                }
                return ans;
            }
        }

        private static String Utf8ToString(IntPtr p)
        {
            if (p == IntPtr.Zero) return "";
            int length = 0;
            unsafe
            {
                byte* buffer = (byte*)p;
                while (*buffer++ != 0) ++length;
            }
            byte[] bytes = new byte[length];
            Marshal.Copy(p, bytes, 0, length);
            return Encoding.UTF8.GetString(bytes);
        }

        [DllImport(Dll.Filename)]
        private static extern void SherpaOnnxDestroyOfflineTtsGeneratedAudio(IntPtr handle);

        [DllImport(Dll.Filename)]
        private static extern int SherpaOnnxWriteWave(IntPtr samples, int n, int sample_rate, [MarshalAs(UnmanagedType.LPArray, ArraySubType = UnmanagedType.I1)] byte[] utf8Filename);
    }

    public class TermAlignment
    {
        public TermAlignment(String text, String phoneme, float startTs, float endTs)
        {
            Text = text;
            Phoneme = phoneme;
            StartTs = startTs;
            EndTs = endTs;
        }

        public String Text { get; }
        public String Phoneme { get; }
        public float StartTs { get; }
        public float EndTs { get; }
    }
}
