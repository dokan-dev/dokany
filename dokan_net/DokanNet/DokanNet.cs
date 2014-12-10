using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;

namespace Dokan
{
    public class DokanOptions
    {
        public ushort Version;
        public ushort ThreadCount;
        public bool DebugMode;
        public bool UseStdErr;
        public bool UseAltStream;
        public bool UseKeepAlive;
        public bool NetworkDrive;
        public bool RemovableDrive;
        public string VolumeLabel;
        public string FileSystemName;
        public string MountPoint;
    }


    // this struct must be the same layout as DOKAN_OPERATIONS
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct DOKAN_OPERATIONS
    {
        public Proxy.CreateFileDelegate CreateFile;
        public Proxy.OpenDirectoryDelegate OpenDirectory;
        public Proxy.CreateDirectoryDelegate CreateDirectory;
        public Proxy.CleanupDelegate Cleanup;
        public Proxy.CloseFileDelegate CloseFile;
        public Proxy.ReadFileDelegate ReadFile;
        public Proxy.WriteFileDelegate WriteFile;
        public Proxy.FlushFileBuffersDelegate FlushFileBuffers;
        public Proxy.GetFileInformationDelegate GetFileInformation;
        public Proxy.FindFilesDelegate FindFiles;
        public IntPtr FindFilesWithPattern; // this is not used in DokanNet
        public Proxy.SetFileAttributesDelegate SetFileAttributes;
        public Proxy.SetFileTimeDelegate SetFileTime;
        public Proxy.DeleteFileDelegate DeleteFile;
        public Proxy.DeleteDirectoryDelegate DeleteDirectory;
        public Proxy.MoveFileDelegate MoveFile;
        public Proxy.SetEndOfFileDelegate SetEndOfFile;
        public Proxy.SetAllocationSizeDelegate SetAllocationSize;
        public Proxy.LockFileDelegate LockFile;
        public Proxy.UnlockFileDelegate UnlockFile;
        public Proxy.GetDiskFreeSpaceDelegate GetDiskFreeSpace;
        public Proxy.GetVolumeInformationDelegate GetVolumeInformation;
        public Proxy.UnmountDelegate Unmount;
        public Proxy.GetFileSecurityDelegate GetFileSecurity;
        public Proxy.SetFileSecurityDelegate SetFileSecurity;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto, Pack = 4)]
    struct DOKAN_OPTIONS
    {
        public ushort Version;
        public ushort ThreadCount; // number of threads to be used
        public uint Options;
        public ulong Dummy1;
        [MarshalAs(UnmanagedType.LPWStr)]
        public string MountPoint;
    }


    class Dokan
    {
        [DllImport("dokan.dll")]
        public static extern int DokanMain(ref DOKAN_OPTIONS options, ref DOKAN_OPERATIONS operations);

        [DllImport("dokan.dll")]
        public static extern int DokanUnmount(int driveLetter);

        [DllImport("dokan.dll")]
        public static extern int DokanRemoveMountPoint(
            [MarshalAs(UnmanagedType.LPWStr)] string mountPoint);

        [DllImport("dokan.dll")]
        public static extern uint DokanVersion();

        [DllImport("dokan.dll")]
        public static extern uint DokanDriverVersion();

        [DllImport("dokan.dll")]
        public static extern bool DokanResetTimeout(uint timeout, ref DOKAN_FILE_INFO rawFileInfo);
    }


    public class DokanNet
    {
        public const int ERROR_FILE_NOT_FOUND       = 2;
        public const int ERROR_PATH_NOT_FOUND       = 3;
        public const int ERROR_ACCESS_DENIED        = 5;
        public const int ERROR_SHARING_VIOLATION    = 32;
        public const int ERROR_INVALID_NAME         = 123;
        public const int ERROR_FILE_EXISTS          = 80;
        public const int ERROR_ALREADY_EXISTS       = 183;

        public const int DOKAN_SUCCESS              = 0;
        public const int DOKAN_ERROR                = -1; // General Error
        public const int DOKAN_DRIVE_LETTER_ERROR   = -2; // Bad Drive letter
        public const int DOKAN_DRIVER_INSTALL_ERROR = -3; // Can't install driver
        public const int DOKAN_START_ERROR          = -4; // Driver something wrong
        public const int DOKAN_MOUNT_ERROR          = -5; // Can't assign drive letter
        public const int DOKAN_MOUNT_POINT_ERROR    = -6; // Mount point is invalid 

        public const int DOKAN_VERSION = 600; // ver 0.6.0

        private const uint DOKAN_OPTION_DEBUG = 1;
        private const uint DOKAN_OPTION_STDERR = 2;
        private const uint DOKAN_OPTION_ALT_STREAM = 4;
        private const uint DOKAN_OPTION_KEEP_ALIVE = 8;
        private const uint DOKAN_OPTION_NETWORK = 16;
        private const uint DOKAN_OPTION_REMOVABLE = 32;

        public static int DokanMain(DokanOptions options, DokanOperations operations)
        {
            if (options.VolumeLabel == null)
            {
                options.VolumeLabel = "DOKAN";
            }
            if (options.FileSystemName == null)
            {
                options.FileSystemName = "Dokan";
            }

            Proxy proxy = new Proxy(options, operations);

            DOKAN_OPTIONS dokanOptions = new DOKAN_OPTIONS();

            dokanOptions.Version = options.Version;
            if (dokanOptions.Version == 0)
            {
                dokanOptions.Version = DOKAN_VERSION;
            }
            dokanOptions.ThreadCount = options.ThreadCount;
            dokanOptions.Options |= options.DebugMode ? DOKAN_OPTION_DEBUG : 0;
            dokanOptions.Options |= options.UseStdErr ? DOKAN_OPTION_STDERR : 0;
            dokanOptions.Options |= options.UseAltStream ? DOKAN_OPTION_ALT_STREAM : 0;
            dokanOptions.Options |= options.UseKeepAlive ? DOKAN_OPTION_KEEP_ALIVE : 0;
            dokanOptions.Options |= options.NetworkDrive ? DOKAN_OPTION_NETWORK : 0;
            dokanOptions.Options |= options.RemovableDrive ? DOKAN_OPTION_REMOVABLE : 0;
            dokanOptions.MountPoint = options.MountPoint;

            DOKAN_OPERATIONS dokanOperations = new DOKAN_OPERATIONS();
            dokanOperations.CreateFile = proxy.CreateFileProxy;
            dokanOperations.OpenDirectory = proxy.OpenDirectoryProxy;
            dokanOperations.CreateDirectory = proxy.CreateDirectoryProxy;
            dokanOperations.Cleanup = proxy.CleanupProxy;
            dokanOperations.CloseFile = proxy.CloseFileProxy;
            dokanOperations.ReadFile = proxy.ReadFileProxy;
            dokanOperations.WriteFile = proxy.WriteFileProxy;
            dokanOperations.FlushFileBuffers = proxy.FlushFileBuffersProxy;
            dokanOperations.GetFileInformation = proxy.GetFileInformationProxy;
            dokanOperations.FindFiles = proxy.FindFilesProxy;
            dokanOperations.SetFileAttributes = proxy.SetFileAttributesProxy;
            dokanOperations.SetFileTime = proxy.SetFileTimeProxy;
            dokanOperations.DeleteFile = proxy.DeleteFileProxy;
            dokanOperations.DeleteDirectory = proxy.DeleteDirectoryProxy;
            dokanOperations.MoveFile = proxy.MoveFileProxy;
            dokanOperations.SetEndOfFile = proxy.SetEndOfFileProxy;
            dokanOperations.SetAllocationSize = proxy.SetAllocationSizeProxy;
            dokanOperations.LockFile = proxy.LockFileProxy;
            dokanOperations.UnlockFile = proxy.UnlockFileProxy;
            dokanOperations.GetDiskFreeSpace = proxy.GetDiskFreeSpaceProxy;           
            dokanOperations.GetVolumeInformation = proxy.GetVolumeInformationProxy;        
            dokanOperations.Unmount = proxy.UnmountProxy;

            return Dokan.DokanMain(ref dokanOptions, ref dokanOperations);
        }


        public static int DokanUnmount(char driveLetter)
        {
            return Dokan.DokanUnmount(driveLetter);
        }

        public static int DokanRemoveMountPoint(string mountPoint)
        {
            return Dokan.DokanRemoveMountPoint(mountPoint);
        }

        public static uint DokanVersion()
        {
            return Dokan.DokanVersion();
        }

        public static uint DokanDriverVersion()
        {
            return Dokan.DokanDriverVersion();
        }

        public static bool DokanResetTimeout(uint timeout, DokanFileInfo fileinfo)
        {
            DOKAN_FILE_INFO rawFileInfo = new DOKAN_FILE_INFO();
            rawFileInfo.DokanContext = fileinfo.DokanContext;
            return Dokan.DokanResetTimeout(timeout, ref rawFileInfo);
        }
    }
}
