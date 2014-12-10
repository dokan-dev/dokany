using System;
using System.Collections.Generic;
using System.IO;
using System.Collections;
using Dokan;

namespace DokanSSHFS
{
    class CacheEntry
    {
        public string Name = null;
        public Dictionary<string, CacheEntry> Children = null;

        public int CreateFileRet = int.MaxValue;
        public int OpenDirectoryRet = int.MaxValue;
        public int GetFileInfoRet = int.MaxValue;
        public FileInformation GetFileInfoValue = null;
        public int FindFilesRet = int.MaxValue;
        public ArrayList FindFilesValue = null;
        public CacheEntry Parrent = null;

        public CacheEntry(string name)
        {
            Name = name;
            Parrent = this;
        }

        public CacheEntry Lookup(string fullname)
        {
            string[] names = fullname.Split('\\');

            CacheEntry current = this;
            CacheEntry child = null;
            foreach (string entry in names)
            {
                if (current.Children == null)
                    current.Children = new Dictionary<string, CacheEntry>();

                if (current.Children.TryGetValue(entry, out child))
                {
                    current = child;
                }
                else
                {
                    CacheEntry cache = new CacheEntry(entry);
                    current.Children[entry] = cache;
                    cache.Parrent = current;
                    current = cache;
                }
            }

            return current;
        }

        public void RemoveCreateFileCache()
        {
            CreateFileRet = int.MaxValue;
        }

        public void RemoveOpenDirectoryCache()
        {
            OpenDirectoryRet = int.MaxValue;
        }

        public void RemoveGetFileInfoCache()
        {
            GetFileInfoRet = int.MaxValue;
            GetFileInfoValue = null;
        }

        public void RemoveFindFilesCache()
        {
            System.Diagnostics.Debug.WriteLine("RemoveFindFilesCache " + Name);
            FindFilesRet = int.MaxValue;
            FindFilesValue = null;
            Children = null;
        }

        public void RemoveAllCache()
        {
            RemoveCreateFileCache();
            RemoveFindFilesCache();
            RemoveGetFileInfoCache();
            RemoveOpenDirectoryCache();
            Children = null;
        }
    }



    class CacheOperations : DokanOperations
    {
        DokanOperations ope_;
        CacheEntry cache_;

        public CacheOperations(DokanOperations ope)
        {
            ope_ = ope;
            cache_ = new CacheEntry(null);
        }


        public int CreateFile(string filename, FileAccess access, FileShare share,
            FileMode mode, FileOptions options, DokanFileInfo info)
        {
            int ret = 0;

            if (filename.EndsWith(":SSHFSProperty.Cache"))
            {
                System.Diagnostics.Debug.WriteLine("SSHFS.Cache: " + filename);

                filename = filename.Remove(filename.IndexOf(":SSHFSProperty.Cache"));
                CacheEntry entry = cache_.Lookup(filename);
                entry.RemoveAllCache();
                return 0;
            }

            if (mode == FileMode.Open || mode == FileMode.OpenOrCreate)
            {
                CacheEntry entry = cache_.Lookup(filename);

                if (mode == FileMode.OpenOrCreate)
                {
                    if (entry.Parrent != null)
                        entry.Parrent.RemoveFindFilesCache();
                }

                if (entry.CreateFileRet == int.MaxValue)
                {
                    ret = ope_.CreateFile(filename, access, share, mode, options, info);
                    entry.CreateFileRet = ret;
                }
                else
                {
                    ret = entry.CreateFileRet;
                }
            }
            else
            {
                ret = ope_.CreateFile(filename, access, share, mode, options, info);

                if (mode == FileMode.Create || mode == FileMode.CreateNew)
                {
                    CacheEntry entry = cache_.Lookup(filename);
                    if (entry.Parrent != null)
                        entry.Parrent.RemoveFindFilesCache();
                }
            }
            return ret;
        }


        public int OpenDirectory(string filename, DokanFileInfo info)
        {
            int ret = 0;

            CacheEntry entry = cache_.Lookup(filename);
            if (entry.OpenDirectoryRet == int.MaxValue)
            {
                ret = ope_.OpenDirectory(filename, info);
                entry.OpenDirectoryRet = ret;
            }
            else
            {
                ret = entry.OpenDirectoryRet;
            }
            return ret;
        }

        public int CreateDirectory(string filename, DokanFileInfo info)
        {
            CacheEntry entry = cache_.Lookup(filename);

            if (entry.Parrent != null)
            {
                entry.Parrent.RemoveAllCache();
            }
            return ope_.CreateDirectory(filename, info);
        }

        public int Cleanup(string filename, DokanFileInfo info)
        {
            return ope_.Cleanup(filename, info);
        }

        public int CloseFile(string filename, DokanFileInfo info)
        {
            return ope_.CloseFile(filename, info);
        }

        public int ReadFile(string filename, byte[] buffer,
            ref uint readBytes, long offset, DokanFileInfo info)
        {
            return ope_.ReadFile(filename, buffer, ref readBytes, offset, info);
        }

        public int WriteFile(string filename, byte[] buffer,
            ref uint writtenBytes, long offset, DokanFileInfo info)
        {
            return ope_.WriteFile(filename, buffer, ref writtenBytes, offset, info);
        }

        public int FlushFileBuffers(string filename, DokanFileInfo info)
        {
            return ope_.FlushFileBuffers(filename, info);
        }


        public int GetFileInformation(string filename, FileInformation fileinfo, DokanFileInfo info)
        {
            CacheEntry entry = cache_.Lookup(filename);

            int ret = 0;

            if (entry.GetFileInfoRet == int.MaxValue)
            {
                ret = ope_.GetFileInformation(filename, fileinfo, info);
                entry.GetFileInfoRet = ret;
                entry.GetFileInfoValue = fileinfo;
            }
            else
            {
                FileInformation finfo = entry.GetFileInfoValue;

                fileinfo.Attributes = finfo.Attributes;
                fileinfo.CreationTime = finfo.CreationTime;
                fileinfo.FileName = finfo.FileName;
                fileinfo.LastAccessTime = finfo.LastAccessTime;
                fileinfo.LastWriteTime = finfo.LastWriteTime;
                fileinfo.Length = finfo.Length;

                ret = entry.GetFileInfoRet;
            }

            return ret;
        }


        public int FindFiles(string filename, ArrayList files, DokanFileInfo info)
        {
            CacheEntry entry = cache_.Lookup(filename);

            int ret = 0;

            if (entry.FindFilesRet == int.MaxValue)
            {
                ret = ope_.FindFiles(filename, files, info);
                entry.FindFilesRet = ret;
                entry.FindFilesValue = files;
            }
            else
            {
                ArrayList cfiles = entry.FindFilesValue;
                foreach (object e in cfiles)
                {
                    files.Add(e);
                }

                ret = entry.FindFilesRet;
            }
            return ret;
        }

        public int SetFileAttributes(string filename, FileAttributes attr, DokanFileInfo info)
        {
            CacheEntry entry = cache_.Lookup(filename);
            entry.RemoveGetFileInfoCache();

            return ope_.SetFileAttributes(filename, attr, info);
        }

        public int SetFileTime(string filename, DateTime ctime, DateTime atime,
            DateTime mtime, DokanFileInfo info)
        {
            CacheEntry entry = cache_.Lookup(filename);
            entry.RemoveGetFileInfoCache();

            return ope_.SetFileTime(filename, ctime, atime, mtime, info);
        }

        public int DeleteFile(string filename, DokanFileInfo info)
        {
            CacheEntry entry = cache_.Lookup(filename);

            entry.RemoveAllCache();
            entry.Parrent.RemoveFindFilesCache();

            return ope_.DeleteFile(filename, info);
        }

        public int DeleteDirectory(string filename, DokanFileInfo info)
        {
            CacheEntry entry = cache_.Lookup(filename);

            entry.RemoveAllCache();
            entry.Parrent.RemoveFindFilesCache();

            return ope_.DeleteDirectory(filename, info);
        }

        public int MoveFile(string filename, string newname, bool replace, DokanFileInfo info)
        {
            CacheEntry entry = cache_.Lookup(filename);

            entry.RemoveAllCache();
            entry.Parrent.RemoveFindFilesCache();

            entry = cache_.Lookup(newname);
            entry.RemoveAllCache();
            entry.Parrent.RemoveFindFilesCache();

            return ope_.MoveFile(filename, newname, replace, info);
        }

        public int SetEndOfFile(string filename, long length, DokanFileInfo info)
        {
            CacheEntry entry = cache_.Lookup(filename);
            entry.RemoveGetFileInfoCache();

            return ope_.SetEndOfFile(filename, length, info);
        }

        public int SetAllocationSize(string filename, long length, DokanFileInfo info)
        {
            CacheEntry entry = cache_.Lookup(filename);
            entry.RemoveGetFileInfoCache();

            return ope_.SetAllocationSize(filename, length, info);
        }

        public int LockFile(string filename, long offset, long length, DokanFileInfo info)
        {
            return ope_.LockFile(filename, offset, length, info);
        }

        public int UnlockFile(string filename, long offset, long length, DokanFileInfo info)
        {
            return ope_.UnlockFile(filename, offset, length, info);
        }

        public int GetDiskFreeSpace(
            ref ulong freeBytesAvailable,
            ref ulong totalBytes,
            ref ulong totalFreeBytes,
            DokanFileInfo info)
        {
            return ope_.GetDiskFreeSpace(ref freeBytesAvailable, ref totalBytes, ref totalFreeBytes, info);
        }

        public int Unmount(DokanFileInfo info)
        {
            cache_.RemoveAllCache();

            return ope_.Unmount(info);
        }


    }
}
