param(
    [Parameter(Mandatory=$true)][string] $MountPath,
    [ValidateRange(1, 100000)][int] $ConflictingReadCount = 128
)

$ErrorActionPreference = 'Stop'

if (-not ('DokanByteRangeLockTest' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class DokanByteRangeLockTest
{
    // Page-aligned storage and 4 KiB I/O multiples also exercise no-buffering.
    const uint BlockSize = 4096;
    const uint LargeWriteSize = 64 * 1024;
    const uint OverlappedFlag = 0x40000000;
    const uint NoBufferingFlag = 0x20000000;
    const int LockViolation = 33;
    const int IoPending = 997;

    [StructLayout(LayoutKind.Sequential)]
    struct Overlapped
    {
        public IntPtr Internal;
        public IntPtr InternalHigh;
        public uint Offset;
        public uint OffsetHigh;
        public IntPtr Event;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFile(
        string path, uint access, uint share, IntPtr security, uint disposition,
        uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetFilePointerEx(
        SafeFileHandle file, long distance, IntPtr newPosition, uint method);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetEndOfFile(SafeFileHandle file);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileSizeEx(SafeFileHandle file, out long size);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool LockFileEx(SafeFileHandle file, uint flags, uint reserved,
        uint lengthLow, uint lengthHigh, IntPtr overlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool UnlockFileEx(SafeFileHandle file, uint reserved,
        uint lengthLow, uint lengthHigh, IntPtr overlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool WriteFile(SafeFileHandle file, IntPtr buffer, uint count,
        out uint transferred, IntPtr overlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool ReadFile(SafeFileHandle file, IntPtr buffer, uint count,
        out uint transferred, IntPtr overlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetOverlappedResult(SafeFileHandle file, IntPtr overlapped,
        out uint transferred, bool wait);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size,
        uint allocationType, uint protection);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool VirtualFree(IntPtr address, UIntPtr size, uint freeType);

    static void Check(bool success, string operation)
    {
        if (!success)
            throw new Win32Exception(Marshal.GetLastWin32Error(), operation);
    }

    static SafeFileHandle Open(string path, uint disposition, uint flags)
    {
        const uint GenericReadWrite = 0xC0000000;
        const uint ShareReadWriteDelete = 7;
        var file = CreateFile(path, GenericReadWrite, ShareReadWriteDelete,
            IntPtr.Zero, disposition, flags, IntPtr.Zero);
        if (file.IsInvalid)
        {
            int error = Marshal.GetLastWin32Error();
            file.Dispose();
            throw new Win32Exception(error, "CreateFile " + path);
        }
        return file;
    }

    static void SetOverlappedOffset(IntPtr overlapped, long offset)
    {
        var operation = new Overlapped
        {
            Offset = unchecked((uint)offset),
            OffsetHigh = unchecked((uint)(offset >> 32))
        };
        Marshal.StructureToPtr(operation, overlapped, false);
    }

    static void ExpectTransfer(SafeFileHandle file, bool write, long offset,
        uint count, IntPtr buffer, IntPtr overlapped, int expectedError)
    {
        if (overlapped == IntPtr.Zero)
            Check(SetFilePointerEx(file, offset, IntPtr.Zero, 0), "SetFilePointerEx");
        else
            SetOverlappedOffset(overlapped, offset);

        uint transferred;
        bool success = write
            ? WriteFile(file, buffer, count, out transferred, overlapped)
            : ReadFile(file, buffer, count, out transferred, overlapped);
        int error = success ? 0 : Marshal.GetLastWin32Error();
        if (overlapped != IntPtr.Zero && (success || error == IoPending))
        {
            success = GetOverlappedResult(file, overlapped, out transferred, true);
            error = success ? 0 : Marshal.GetLastWin32Error();
        }
        if (error != expectedError || (success && transferred != count))
            throw new InvalidOperationException(String.Format(
                "{0} at offset {1} for {2} bytes returned Win32 error {3} " +
                "and {4} bytes. Expected Win32 error {5}.",
                write ? "Write" : "Read", offset, count, error, transferred, expectedError));
    }

    static void RunCase(string directory, uint flags, IntPtr buffer,
        IntPtr overlapped, int conflictingReadCount)
    {
        const long FileSize = 2 * LargeWriteSize;
        const uint LockOffset = BlockSize;
        const uint FailImmediately = 1;
        const uint Exclusive = 2;
        string path = Path.Combine(directory, "dokan-lock-" + Guid.NewGuid().ToString("N"));
        var owner = Open(path, 1 /* CREATE_NEW */, 0);
        try
        {
            using (owner)
            using (var other = Open(path, 3 /* OPEN_EXISTING */, flags))
            {
                Check(SetFilePointerEx(owner, FileSize, IntPtr.Zero, 0), "SetFilePointerEx");
                Check(SetEndOfFile(owner), "SetEndOfFile");
                var io = (flags & OverlappedFlag) != 0 ? overlapped : IntPtr.Zero;
                uint count = (flags & NoBufferingFlag) != 0 ? BlockSize : 10;
                long offset = LockOffset + ((flags & NoBufferingFlag) != 0 ? 0 : 100);

                SetOverlappedOffset(overlapped, LockOffset);
                Check(LockFileEx(owner, FailImmediately | Exclusive, 0, BlockSize, 0,
                    overlapped), "LockFileEx exclusive");
                try
                {
                    // Repeated read conflicts exercise event cleanup under Driver Verifier.
                    for (int i = 0; i < conflictingReadCount; ++i)
                        ExpectTransfer(other, false, offset, count, buffer, io, LockViolation);
                    ExpectTransfer(other, true, offset, count, buffer, io, LockViolation);
                    // This crosses the locked range and exceeds EVENT_CONTEXT_MAX_SIZE.
                    ExpectTransfer(other, true, 0, LargeWriteSize, buffer, io, LockViolation);
                    ExpectTransfer(owner, true, LockOffset, BlockSize, buffer, IntPtr.Zero, 0);
                    ExpectTransfer(other, true, LockOffset + BlockSize, BlockSize, buffer, io, 0);
                }
                finally
                {
                    SetOverlappedOffset(overlapped, LockOffset);
                    Check(UnlockFileEx(owner, 0, BlockSize, 0, overlapped), "UnlockFileEx");
                }

                ExpectTransfer(other, true, offset, count, buffer, io, 0);

                SetOverlappedOffset(overlapped, LockOffset);
                Check(LockFileEx(owner, FailImmediately, 0, BlockSize, 0,
                    overlapped), "LockFileEx shared");
                try
                {
                    ExpectTransfer(other, false, offset, count, buffer, io, 0);
                    ExpectTransfer(other, true, offset, count, buffer, io, LockViolation);
                    ExpectTransfer(owner, true, offset, count, buffer, IntPtr.Zero, LockViolation);
                }
                finally
                {
                    SetOverlappedOffset(overlapped, LockOffset);
                    Check(UnlockFileEx(owner, 0, BlockSize, 0, overlapped), "UnlockFileEx");
                }

                if (io != IntPtr.Zero)
                {
                    ExpectTransfer(other, true, -1, BlockSize, buffer, io, 0);
                    long size;
                    Check(GetFileSizeEx(other, out size), "GetFileSizeEx");
                    if (size != FileSize + BlockSize)
                        throw new InvalidOperationException("The append did not extend the file.");
                }
            }
        }
        finally
        {
            File.Delete(path);
        }
    }

    public static void Run(string directory, int conflictingReadCount)
    {
        const uint CommitReserve = 0x3000;
        const uint PageReadWrite = 4;
        const uint Release = 0x8000;
        IntPtr buffer = VirtualAlloc(IntPtr.Zero, new UIntPtr(LargeWriteSize),
            CommitReserve, PageReadWrite);
        Check(buffer != IntPtr.Zero, "VirtualAlloc");
        IntPtr overlapped = IntPtr.Zero;
        try
        {
            overlapped = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(Overlapped)));
            foreach (uint flags in new uint[] { 0, OverlappedFlag, NoBufferingFlag,
                OverlappedFlag | NoBufferingFlag })
            {
                Console.WriteLine("Testing byte-range locks with file flags 0x{0:X8}", flags);
                RunCase(directory, flags, buffer, overlapped, conflictingReadCount);
            }
        }
        finally
        {
            Marshal.FreeHGlobal(overlapped);
            Check(VirtualFree(buffer, UIntPtr.Zero, Release), "VirtualFree");
        }
    }
}
'@
}

[DokanByteRangeLockTest]::Run($MountPath, $ConflictingReadCount)
Write-Host "Byte-range lock tests passed" -ForegroundColor Green
