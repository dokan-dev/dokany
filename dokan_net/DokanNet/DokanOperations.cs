using System;
using System.IO;
using System.Collections;

namespace Dokan
{
    public class DokanFileInfo
    {
        public Object Context;
        public bool IsDirectory;
        public ulong InfoId;
        public uint ProcessId;
        public bool DeleteOnClose;
        public bool PagingIo;
        public bool SynchronousIo;
        public bool Nocache;
        public bool WriteToEndOfFile;
        public readonly ulong DokanContext; // for internal use

        public DokanFileInfo(ulong dokanContext)
        {
            Context = null;
            IsDirectory = false;
            DeleteOnClose = false;
            PagingIo = false;
            SynchronousIo = false;
            Nocache = false;
            WriteToEndOfFile = false;
            InfoId = 0;
            DokanContext = dokanContext;
        }
    }


    public class FileInformation
    {
        public FileAttributes Attributes;
        public DateTime CreationTime;
        public DateTime LastAccessTime;
        public DateTime LastWriteTime;
        public long Length;
        public string FileName;
        /*
        public FileInformation()
        {
            Attributes = FileAttributes.Normal;
            Length = 0;
        }
         */
    }

    public interface DokanOperations
    {
        int CreateFile(
                string filename,
                FileAccess access,
                FileShare share,
                FileMode mode,
                FileOptions options,
                DokanFileInfo info);

        int OpenDirectory(
                string filename,
                DokanFileInfo info);

        int CreateDirectory(
                string filename,
                DokanFileInfo info);

        int Cleanup(
                string filename,
                DokanFileInfo info);

        int CloseFile(
                string filename,
                DokanFileInfo info);

        int ReadFile(
                string filename,
                byte[] buffer,
                ref uint readBytes,
                long offset,
                DokanFileInfo info);

        int WriteFile(
                string filename,
                byte[] buffer,
                ref uint writtenBytes,
                long offset,
                DokanFileInfo info);

        int FlushFileBuffers(
                string filename,
                DokanFileInfo info);

        int GetFileInformation(
                string filename,
                FileInformation fileinfo,
                DokanFileInfo info);

        int FindFiles(
                string filename,
                ArrayList files,
                DokanFileInfo info);

        int SetFileAttributes(
                string filename,
                FileAttributes attr,
                DokanFileInfo info);

        int SetFileTime(
                string filename,
                DateTime ctime,
                DateTime atime,
                DateTime mtime,
                DokanFileInfo info);

        int DeleteFile(
                string filename,
                DokanFileInfo info);

        int DeleteDirectory(
                string filename,
                DokanFileInfo info);

        int MoveFile(
                string filename,
                string newname,
                bool replace,
                DokanFileInfo info);

        int SetEndOfFile(
                string filename,
                long length,
                DokanFileInfo info);

        int SetAllocationSize(
                string filename,
                long length,
                DokanFileInfo info);

        int LockFile(
                string filename,
                long offset,
                long length,
                DokanFileInfo info);

        int UnlockFile(
                string filename,
                long offset,
                long length,
                DokanFileInfo info);

        int GetDiskFreeSpace(
                ref ulong freeBytesAvailable,
                ref ulong totalBytes,
                ref ulong totalFreeBytes,
                DokanFileInfo info);

        int Unmount(
                DokanFileInfo info);

    }
}
