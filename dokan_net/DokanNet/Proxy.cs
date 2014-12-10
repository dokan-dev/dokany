using System;
using System.Collections;
using System.Collections.Generic;
using System.Text;
using System.IO;
using System.Runtime.InteropServices;

using ComTypes = System.Runtime.InteropServices.ComTypes;

namespace Dokan
{
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct BY_HANDLE_FILE_INFORMATION
    {
        public uint dwFileAttributes;
        public ComTypes.FILETIME ftCreationTime;
        public ComTypes.FILETIME ftLastAccessTime;
        public ComTypes.FILETIME ftLastWriteTime;
        public uint dwVolumeSerialNumber;
        public uint nFileSizeHigh;
        public uint nFileSizeLow;
        public uint dwNumberOfLinks;
        public uint nFileIndexHigh;
        public uint nFileIndexLow;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct DOKAN_FILE_INFO
    {
        public ulong Context;
        public ulong DokanContext;
        public IntPtr DokanOptions;
        public uint ProcessId;
        public byte IsDirectory;
        public byte DeleteOnClose;
        public byte PagingIo;
        public byte SynchronousIo;
        public byte Nocache;
        public byte WriteToEndOfFile;
    }

    [Flags]
    enum SECURITY_INFORMATION : uint
    {
        OWNER_SECURITY_INFORMATION = 0x00000001,
        GROUP_SECURITY_INFORMATION = 0x00000002,
        DACL_SECURITY_INFORMATION = 0x00000004,
        SACL_SECURITY_INFORMATION = 0x00000008,
        UNPROTECTED_SACL_SECURITY_INFORMATION = 0x10000000,
        UNPROTECTED_DACL_SECURITY_INFORMATION = 0x20000000,
        PROTECTED_SACL_SECURITY_INFORMATION = 0x40000000,
        PROTECTED_DACL_SECURITY_INFORMATION = 0x80000000
    }

    [StructLayoutAttribute(LayoutKind.Sequential, Pack = 4)]
    struct SECURITY_DESCRIPTOR
    {
        public byte revision;
        public byte size;
        public short control;
        public IntPtr owner;
        public IntPtr group;
        public IntPtr sacl;
        public IntPtr dacl;
    }

    class Proxy
    {
        private DokanOperations operations_;
        private ArrayList array_;
        private Dictionary<ulong, DokanFileInfo> infoTable_;
        private ulong infoId_ = 0;
        private object infoTableLock_ = new object();
        private DokanOptions options_;

        public Proxy(DokanOptions options, DokanOperations operations)
        {
            operations_ = operations;
            options_ = options;
            array_ = new ArrayList();
            infoTable_ = new Dictionary<ulong, DokanFileInfo>();
        }

        private void ConvertFileInfo(ref DOKAN_FILE_INFO rawInfo, DokanFileInfo info)
        {
            info.IsDirectory = rawInfo.IsDirectory == 1;
            info.ProcessId = rawInfo.ProcessId;
            info.PagingIo = rawInfo.PagingIo == 1;
            info.DeleteOnClose = rawInfo.DeleteOnClose == 1;
            info.SynchronousIo = rawInfo.SynchronousIo == 1;
            info.Nocache = rawInfo.Nocache == 1;
            info.WriteToEndOfFile = rawInfo.WriteToEndOfFile == 1;
        }

        private DokanFileInfo GetNewFileInfo(ref DOKAN_FILE_INFO rawFileInfo)
        {
            DokanFileInfo fileInfo = new DokanFileInfo(rawFileInfo.DokanContext);

            lock (infoTableLock_)
            {
                fileInfo.InfoId = ++infoId_;

                rawFileInfo.Context = fileInfo.InfoId;
                ConvertFileInfo(ref rawFileInfo, fileInfo);
                // to avoid GC
                infoTable_[fileInfo.InfoId] = fileInfo;
            }
            return fileInfo;
        }

        private DokanFileInfo GetFileInfo(ref DOKAN_FILE_INFO rawFileInfo)
        {
            DokanFileInfo fileInfo = null;
            lock (infoTableLock_)
            {
                if (rawFileInfo.Context != 0)
                {
                    infoTable_.TryGetValue(rawFileInfo.Context, out fileInfo);
                }

                if (fileInfo == null)
                {
                    // bug?
                    fileInfo = new DokanFileInfo(rawFileInfo.DokanContext);
                }
                ConvertFileInfo(ref rawFileInfo, fileInfo);
            }
            return fileInfo;
        }
      
        private string GetFileName(IntPtr fileName)
        {
            return Marshal.PtrToStringUni(fileName);
        }


        private const uint GENERIC_READ = 0x80000000;
        private const uint GENERIC_WRITE = 0x40000000;
        private const uint GENERIC_EXECUTE = 0x20000000;
        
        private const uint FILE_READ_DATA = 0x0001;
        private const uint FILE_READ_ATTRIBUTES = 0x0080;
        private const uint FILE_READ_EA = 0x0008;
        private const uint FILE_WRITE_DATA = 0x0002;
        private const uint FILE_WRITE_ATTRIBUTES = 0x0100;
        private const uint FILE_WRITE_EA = 0x0010;

        private const uint FILE_SHARE_READ = 0x00000001;
        private const uint FILE_SHARE_WRITE = 0x00000002;
        private const uint FILE_SHARE_DELETE = 0x00000004;

        private const uint CREATE_NEW = 1;
        private const uint CREATE_ALWAYS = 2;
        private const uint OPEN_EXISTING = 3;
        private const uint OPEN_ALWAYS = 4;
        private const uint TRUNCATE_EXISTING = 5;
        
        private const uint FILE_ATTRIBUTE_ARCHIVE = 0x00000020;
        private const uint FILE_ATTRIBUTE_ENCRYPTED = 0x00004000;
        private const uint FILE_ATTRIBUTE_HIDDEN = 0x00000002;
        private const uint FILE_ATTRIBUTE_NORMAL = 0x00000080;
        private const uint FILE_ATTRIBUTE_NOT_CONTENT_INDEXED = 0x00002000;
        private const uint FILE_ATTRIBUTE_OFFLINE = 0x00001000;
        private const uint FILE_ATTRIBUTE_READONLY = 0x00000001;
        private const uint FILE_ATTRIBUTE_SYSTEM = 0x00000004;
        private const uint FILE_ATTRIBUTE_TEMPORARY = 0x00000100;

        private const uint FILE_FLAG_WRITE_THROUGH = 0x80000000;
        private const uint FILE_FLAG_OVERLAPPED = 0x40000000;
        private const uint FILE_FLAG_RANDOM_ACCESS = 0x10000000;
        private const uint FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000;
        private const uint FILE_FLAG_DELETE_ON_CLOSE = 0x04000000;

        public delegate int CreateFileDelegate(
            IntPtr rawFilName,
            uint rawAccessMode,
            uint rawShare,
            uint rawCreationDisposition,
            uint rawFlagsAndAttributes,
            ref DOKAN_FILE_INFO dokanFileInfo);

        public int CreateFileProxy(
            IntPtr rawFileName,
            uint rawAccessMode,
            uint rawShare,
            uint rawCreationDisposition,
            uint rawFlagsAndAttributes,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                DokanFileInfo info = GetNewFileInfo(ref rawFileInfo);
                
                FileAccess access = FileAccess.Read;
                FileShare share = FileShare.None;
                FileMode mode = FileMode.Open;
                FileOptions options = FileOptions.None;

                if ((rawAccessMode & FILE_READ_DATA) != 0 && (rawAccessMode & FILE_WRITE_DATA) != 0)
                {
                    access = FileAccess.ReadWrite;
                }
                else if ((rawAccessMode & FILE_WRITE_DATA) != 0)
                {
                    access = FileAccess.Write;
                }
                else
                {
                    access = FileAccess.Read;
                }

                if ((rawShare & FILE_SHARE_READ) != 0)
                {
                    share = FileShare.Read;
                }

                if ((rawShare & FILE_SHARE_WRITE) != 0)
                {
                    share |= FileShare.Write;
                }

                if ((rawShare & FILE_SHARE_DELETE) != 0)
                {
                    share |= FileShare.Delete;
                }

                if ((rawFlagsAndAttributes & FILE_FLAG_DELETE_ON_CLOSE) != 0)
                {
                    options |= FileOptions.DeleteOnClose;
                }

                if ((rawFlagsAndAttributes & FILE_FLAG_WRITE_THROUGH) != 0)
                {
                    options |= FileOptions.WriteThrough;
                }

                if ((rawFlagsAndAttributes & FILE_FLAG_SEQUENTIAL_SCAN) != 0)
                {
                    options |= FileOptions.SequentialScan;
                }

                if ((rawFlagsAndAttributes & FILE_FLAG_RANDOM_ACCESS) != 0)
                {
                    options |= FileOptions.RandomAccess;
                }

                if ((rawFlagsAndAttributes & FILE_FLAG_OVERLAPPED) != 0)
                {
                    options |= FileOptions.Asynchronous;
                }
                // TODO: supports FileOptions.Encrypted

                switch (rawCreationDisposition)
                {
                    case CREATE_NEW:
                        mode = FileMode.CreateNew;
                        break;
                    case CREATE_ALWAYS:
                        mode = FileMode.Create;
                        break;
                    case OPEN_EXISTING:
                        mode = FileMode.Open;
                        break;
                    case OPEN_ALWAYS:
                        mode = FileMode.OpenOrCreate;
                        break;
                    case TRUNCATE_EXISTING:
                        mode = FileMode.Truncate;
                        break;
                }

                int ret = operations_.CreateFile(file, access, share, mode, options, info);

                if (info.IsDirectory)
                    rawFileInfo.IsDirectory = 1;

                return ret;
            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -2;
            }

        }

        ////

        public delegate int OpenDirectoryDelegate(
            IntPtr FileName,
            ref DOKAN_FILE_INFO FileInfo);

        public int OpenDirectoryProxy(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                DokanFileInfo info = GetNewFileInfo(ref rawFileInfo);
                return operations_.OpenDirectory(file, info);

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int CreateDirectoryDelegate(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int CreateDirectoryProxy(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                DokanFileInfo info = GetNewFileInfo(ref rawFileInfo);
                return operations_.CreateDirectory(file, info);

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int CleanupDelegate(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int CleanupProxy(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);
                return operations_.Cleanup(file, GetFileInfo(ref rawFileInfo));

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int CloseFileDelegate(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int CloseFileProxy(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);
                DokanFileInfo info = GetFileInfo(ref rawFileInfo);

                int ret = operations_.CloseFile(file, info);

                rawFileInfo.Context = 0;

                lock (infoTableLock_)
                {
                    infoTable_.Remove(info.InfoId);
                }
                return ret;

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int ReadFileDelegate(
            IntPtr rawFileName,
            IntPtr rawBuffer,
            uint rawBufferLength,
            ref uint rawReadLength,
            long rawOffset,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int ReadFileProxy(
            IntPtr rawFileName,
            IntPtr rawBuffer,
            uint rawBufferLength,
            ref uint rawReadLength,
            long rawOffset,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                byte[] buf = new Byte[rawBufferLength];

                uint readLength = 0;
                int ret = operations_.ReadFile(
                    file, buf, ref readLength, rawOffset, GetFileInfo(ref rawFileInfo));
                if (ret == 0)
                {
                    rawReadLength = readLength;
                    Marshal.Copy(buf, 0, rawBuffer, (int)rawBufferLength);
                }
                return ret;

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int WriteFileDelegate(
            IntPtr rawFileName,
            IntPtr rawBuffer,
            uint rawNumberOfBytesToWrite,
            ref uint rawNumberOfBytesWritten,
            long rawOffset,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int WriteFileProxy(
            IntPtr rawFileName,
            IntPtr rawBuffer,
            uint rawNumberOfBytesToWrite,
            ref uint rawNumberOfBytesWritten,
            long rawOffset,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                Byte[] buf = new Byte[rawNumberOfBytesToWrite];
                Marshal.Copy(rawBuffer, buf, 0, (int)rawNumberOfBytesToWrite);

                uint bytesWritten = 0;
                int ret = operations_.WriteFile(
                    file, buf, ref bytesWritten, rawOffset, GetFileInfo(ref rawFileInfo));
                if (ret == 0)
                    rawNumberOfBytesWritten = bytesWritten;
                return ret;

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int FlushFileBuffersDelegate(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int FlushFileBuffersProxy(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);
                int ret = operations_.FlushFileBuffers(file, GetFileInfo(ref rawFileInfo));
                return ret;

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }
       
        ////

        public delegate int GetFileInformationDelegate(
            IntPtr FileName,
            ref BY_HANDLE_FILE_INFORMATION HandleFileInfo,
            ref DOKAN_FILE_INFO FileInfo);

        public int GetFileInformationProxy(
            IntPtr rawFileName,
            ref BY_HANDLE_FILE_INFORMATION rawHandleFileInformation,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                FileInformation fi = new FileInformation();

                int ret = operations_.GetFileInformation(file, fi, GetFileInfo(ref rawFileInfo));

                if (ret == 0)
                {
                    rawHandleFileInformation.dwFileAttributes = (uint)fi.Attributes;

                    rawHandleFileInformation.ftCreationTime.dwHighDateTime =
                        (int)(fi.CreationTime.ToFileTime() >> 32);
                    rawHandleFileInformation.ftCreationTime.dwLowDateTime =
                        (int)(fi.CreationTime.ToFileTime() & 0xffffffff);

                    rawHandleFileInformation.ftLastAccessTime.dwHighDateTime =
                        (int)(fi.LastAccessTime.ToFileTime() >> 32);
                    rawHandleFileInformation.ftLastAccessTime.dwLowDateTime =
                        (int)(fi.LastAccessTime.ToFileTime() & 0xffffffff);

                    rawHandleFileInformation.ftLastWriteTime.dwHighDateTime =
                        (int)(fi.LastWriteTime.ToFileTime() >> 32);
                    rawHandleFileInformation.ftLastWriteTime.dwLowDateTime =
                        (int)(fi.LastWriteTime.ToFileTime() & 0xffffffff);

                    rawHandleFileInformation.nFileSizeLow =
                        (uint)(fi.Length & 0xffffffff);
                    rawHandleFileInformation.nFileSizeHigh =
                        (uint)(fi.Length >> 32);
                }

                return ret;

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }

        }

        ////

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto, Pack = 4)]
        struct WIN32_FIND_DATA
        {
            public FileAttributes dwFileAttributes;
            public ComTypes.FILETIME ftCreationTime;
            public ComTypes.FILETIME ftLastAccessTime;
            public ComTypes.FILETIME ftLastWriteTime;
            public uint nFileSizeHigh;
            public uint nFileSizeLow;
            public uint dwReserved0;
            public uint dwReserved1;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
            public string cFileName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 14)]
            public string cAlternateFileName;
        }

        private delegate int FILL_FIND_DATA(
            ref WIN32_FIND_DATA rawFindData,
            ref DOKAN_FILE_INFO rawFileInfo);

        public delegate int FindFilesDelegate(
            IntPtr rawFileName,
            IntPtr rawFillFindData, // function pointer
            ref DOKAN_FILE_INFO rawFileInfo);

        public int FindFilesProxy(
            IntPtr rawFileName,
            IntPtr rawFillFindData, // function pointer
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                ArrayList files = new ArrayList();
                int ret = operations_.FindFiles(file, files, GetFileInfo(ref rawFileInfo));

                FILL_FIND_DATA fill = (FILL_FIND_DATA)Marshal.GetDelegateForFunctionPointer(
                    rawFillFindData, typeof(FILL_FIND_DATA));

                if (ret == 0)
                {
                    IEnumerator entry = files.GetEnumerator();
                    while (entry.MoveNext())
                    {
                        FileInformation fi = (FileInformation)(entry.Current);
                        WIN32_FIND_DATA data = new WIN32_FIND_DATA();
                        //ZeroMemory(&data, sizeof(WIN32_FIND_DATAW));

                        data.dwFileAttributes = fi.Attributes;

                        data.ftCreationTime.dwHighDateTime =
                            (int)(fi.CreationTime.ToFileTime() >> 32);
                        data.ftCreationTime.dwLowDateTime =
                            (int)(fi.CreationTime.ToFileTime() & 0xffffffff);

                        data.ftLastAccessTime.dwHighDateTime =
                            (int)(fi.LastAccessTime.ToFileTime() >> 32);
                        data.ftLastAccessTime.dwLowDateTime =
                            (int)(fi.LastAccessTime.ToFileTime() & 0xffffffff);

                        data.ftLastWriteTime.dwHighDateTime =
                            (int)(fi.LastWriteTime.ToFileTime() >> 32);
                        data.ftLastWriteTime.dwLowDateTime =
                            (int)(fi.LastWriteTime.ToFileTime() & 0xffffffff);

                        data.nFileSizeLow =
                            (uint)(fi.Length & 0xffffffff);
                        data.nFileSizeHigh =
                            (uint)(fi.Length >> 32);

                        data.cFileName = fi.FileName;

                        fill(ref data, ref rawFileInfo);
                    }

                }
                return ret;

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }

        }

        ////

        public delegate int SetEndOfFileDelegate(
            IntPtr rawFileName,
            long rawByteOffset,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int SetEndOfFileProxy(
            IntPtr rawFileName,
            long rawByteOffset,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                return operations_.SetEndOfFile(file, rawByteOffset, GetFileInfo(ref rawFileInfo));

            }
            catch (Exception e)
            {

                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }


        public delegate int SetAllocationSizeDelegate(
            IntPtr rawFileName,
            long rawLength,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int SetAllocationSizeProxy(
            IntPtr rawFileName,
            long rawLength,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                return operations_.SetAllocationSize(file, rawLength, GetFileInfo(ref rawFileInfo));

            }
            catch (Exception e)
            {

                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }


        ////

        public delegate int SetFileAttributesDelegate(
            IntPtr rawFileName,
            uint rawAttributes,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int SetFileAttributesProxy(
            IntPtr rawFileName,
            uint rawAttributes,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                FileAttributes attr = (FileAttributes)rawAttributes;
                return operations_.SetFileAttributes(file, attr, GetFileInfo(ref rawFileInfo));

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int SetFileTimeDelegate(
            IntPtr rawFileName,
            ref ComTypes.FILETIME rawCreationTime,
            ref ComTypes.FILETIME rawLastAccessTime,
            ref ComTypes.FILETIME rawLastWriteTime,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int SetFileTimeProxy(
            IntPtr rawFileName,
            ref ComTypes.FILETIME rawCreationTime,
            ref ComTypes.FILETIME rawLastAccessTime,
            ref ComTypes.FILETIME rawLastWriteTime,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                long time;

                time = ((long)rawCreationTime.dwHighDateTime << 32) + (uint)rawCreationTime.dwLowDateTime;
                DateTime ctime = DateTime.FromFileTime(time);
                
                if (time == 0)
                    ctime = DateTime.MinValue;

                time = ((long)rawLastAccessTime.dwHighDateTime << 32) + (uint)rawLastAccessTime.dwLowDateTime;
                DateTime atime = DateTime.FromFileTime(time);

                if (time == 0)
                    atime = DateTime.MinValue;

                time = ((long)rawLastWriteTime.dwHighDateTime << 32) + (uint)rawLastWriteTime.dwLowDateTime;
                DateTime mtime = DateTime.FromFileTime(time);

                if (time == 0)
                    mtime = DateTime.MinValue;

                return operations_.SetFileTime(
                    file, ctime, atime, mtime, GetFileInfo(ref rawFileInfo));

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int DeleteFileDelegate(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int DeleteFileProxy(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);

                return operations_.DeleteFile(file, GetFileInfo(ref rawFileInfo));

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int DeleteDirectoryDelegate(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int DeleteDirectoryProxy(
            IntPtr rawFileName,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);
                return operations_.DeleteDirectory(file, GetFileInfo(ref rawFileInfo));

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int MoveFileDelegate(
            IntPtr rawFileName,
            IntPtr rawNewFileName,
            int rawReplaceIfExisting,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int MoveFileProxy(
            IntPtr rawFileName,
            IntPtr rawNewFileName,
            int rawReplaceIfExisting,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);
                string newfile = GetFileName(rawNewFileName);

                return operations_.MoveFile(
                    file, newfile, rawReplaceIfExisting != 0 ? true : false,
                    GetFileInfo(ref rawFileInfo));

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int LockFileDelegate(
            IntPtr rawFileName,
            long rawByteOffset,
            long rawLength,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int LockFileProxy(
            IntPtr rawFileName,
            long rawByteOffset,
            long rawLength,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);
                return operations_.LockFile(
                    file, rawByteOffset, rawLength, GetFileInfo(ref rawFileInfo));

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int UnlockFileDelegate(
            IntPtr rawFileName,
            long rawByteOffset,
            long rawLength,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int UnlockFileProxy(
            IntPtr rawFileName,
            long rawByteOffset,
            long rawLength,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                string file = GetFileName(rawFileName);
                return operations_.UnlockFile(
                    file, rawByteOffset, rawLength, GetFileInfo(ref rawFileInfo));

            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        ////

        public delegate int GetDiskFreeSpaceDelegate(
            ref ulong rawFreeBytesAvailable,
            ref ulong rawTotalNumberOfBytes,
            ref ulong rawTotalNumberOfFreeBytes,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int GetDiskFreeSpaceProxy(
            ref ulong rawFreeBytesAvailable,
            ref ulong rawTotalNumberOfBytes,
            ref ulong rawTotalNumberOfFreeBytes,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                return operations_.GetDiskFreeSpace(
                    ref rawFreeBytesAvailable,
                    ref rawTotalNumberOfBytes,
                    ref rawTotalNumberOfFreeBytes,
                    GetFileInfo(ref rawFileInfo));
            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        public delegate int GetVolumeInformationDelegate(
            IntPtr rawVolumeNameBuffer,
            uint rawVolumeNameSize,
            ref uint rawVolumeSerialNumber,
            ref uint rawMaximumComponentLength,
            ref uint rawFileSystemFlags,
            IntPtr rawFileSystemNameBuffer,
            uint rawFileSystemNameSize,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int GetVolumeInformationProxy(
            IntPtr rawVolumeNameBuffer,
            uint rawVolumeNameSize,
            ref uint rawVolumeSerialNumber,
            ref uint rawMaximumComponentLength,
            ref uint rawFileSystemFlags,
            IntPtr rawFileSystemNameBuffer,
            uint rawFileSystemNameSize,
            ref DOKAN_FILE_INFO FileInfo)
        {
            byte[] volume = System.Text.Encoding.Unicode.GetBytes(options_.VolumeLabel);
            Marshal.Copy(volume, 0, rawVolumeNameBuffer, Math.Min((int)rawVolumeNameSize, volume.Length));
            rawVolumeSerialNumber = 0x19831116;
            rawMaximumComponentLength = 256;

            // FILE_CASE_SENSITIVE_SEARCH | 
            // FILE_CASE_PRESERVED_NAMES |
            // FILE_UNICODE_ON_DISK
            rawFileSystemFlags = 7;

            byte[] sys = System.Text.Encoding.Unicode.GetBytes(options_.FileSystemName);
            Marshal.Copy(sys, 0, rawFileSystemNameBuffer, Math.Min((int)rawFileSystemNameSize, sys.Length));
            return 0;
        }


        public delegate int UnmountDelegate(
            ref DOKAN_FILE_INFO rawFileInfo);

        public int UnmountProxy(
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            try
            {
                return operations_.Unmount(GetFileInfo(ref rawFileInfo));
            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e.ToString());
                return -1;
            }
        }

        public delegate int GetFileSecurityDelegate(
            IntPtr rawFileName,
            ref SECURITY_INFORMATION rawRequestedInformation,
            ref SECURITY_DESCRIPTOR rawSecurityDescriptor,
            uint rawSecurityDescriptorLength,
            ref uint rawSecurityDescriptorLengthNeeded,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int GetFileSecurity(
            IntPtr rawFileName,
            ref SECURITY_INFORMATION rawRequestedInformation,
            ref SECURITY_DESCRIPTOR rawSecurityDescriptor,
            uint rawSecurityDescriptorLength,
            ref uint rawSecurityDescriptorLengthNeeded,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            return -1;
        }

        public delegate int SetFileSecurityDelegate(
            IntPtr rawFileName,
            ref SECURITY_INFORMATION rawSecurityInformation,
            ref SECURITY_DESCRIPTOR rawSecurityDescriptor,
            uint rawSecurityDescriptorLength,
            ref DOKAN_FILE_INFO rawFileInfo);

        public int SetFileSecurity(
            IntPtr rawFileName,
            ref SECURITY_INFORMATION rawSecurityInformation,
            ref SECURITY_DESCRIPTOR rawSecurityDescriptor,
            ref uint rawSecurityDescriptorLengthNeeded,
            ref DOKAN_FILE_INFO rawFileInfo)
        {
            return -1;
        }
    }
}
