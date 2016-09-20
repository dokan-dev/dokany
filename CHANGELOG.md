# Change Log
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/) and this project adheres to [Semantic Versioning](http://semver.org/).

## [Unreleased]

## [1.0.0.5000] - 2016-09-20
### Added
- MAJOR version to binary name
- Resource information to library with full dokan version
- Installer - Move to WiX toolset
- Library - DokanOption to mount the drive on current session only 
- FUSE - Export utils fonctions
- FUSE - `daemon_timeout=<milisec>`, background and network option parameter as options. 
- Library - Notify drive arrival/removal
- Driver - `dokan.inf` to solution - `.cab` file is now build and embedded with installer
- FUSE Cygwin / Msys2 binaries in installer (optional)
- FUSE Cygwin mirror as example
- Driver - log for unknown `IOCTL_STORAGE_QUERY_PROPERTY`
- Mirror - Use control handler function to gracefully close on Ctrl+C
- FUSE - handle `SetAllocationSize`
- Installer - Environement variable to dokan path: `DokanLibrary1` / `DokanLibrary1_LibraryPath_x64` / `DokanLibrary1_LibraryPath_x86`
- Dokan NP - Totally new Dokan network provider
- Driver - Oplock implementation
- Logo Dokan in VSIX
- Library - Parameters on `DokanGetMountPointList` to only retrieve instances with UNC
- Mirror - `MirrorDokanGetDiskFreeSpace` as example
- Driver - Handle `FileFsLabelInformation` (Rename label device name)
- Library - Handle `GetFileSecurity` callbacks for directories.
- Library - Default `QueryVolumeInformation` file system name
- Use WinFSTest as test suits
- Library - Auto add current and parent folder in `FindFiles` request when missing
- Installer - `Install development files` option at installations time
- Installer - NET 4.0 in bundles
- Library - `DokanNtStatusFromWin32` helper
- Library - `FileDispositionInformation` check attribute `FILE_ATTRIBUTE_READONLY`
- Driver - `SL_OPEN_TARGET_DIRECTORY` is now handle directly by the driver
- A website with the [documentation](https://dokan-dev.github.io/dokany-doc/html/).
- Support PagingIO
- FUSE - Use FUSE-compatible dir-hierarchy

### Changed
- Installer - Update redistributable link to VS Update 2
- Installer - Add `dokan.dll` to install folder
- Installer - Move `dokanfuse.dll` to install folder
- Installer - Destination folder have now the dokan version in the name
- Sign binary and installer with SHA1 & SHA2 
- Library - Call FindFiles if FindFilesWithPattern returns `STATUS_NOT_IMPLEMENTED`
- Library - Disable network device when mount point folder is specified. 
- Dokanctl - Register `dokannp dll` from Dokan Library install instead of sys32.
- FUSE - use `_WIN32` in FUSE wrapper instead of `WIN32` define
- Driver - `WriteFile` directly return `STATUS_SUCCESS` when there is nothing to write
- Library - When a delete fail, we now request parent folder if we can delete child
- Library - `DokanRemoveMountPointEx` has now a Safe option (force during remove if safe)
- Dokanctl - No longer need admin permission for listing mount point, show usage and print version
- Mirror - Update show usage
- Driver - Use LookasideLists for DokanCCB and DokanFCB
- FUSE - Improve and fix debug logs
- FUSE - Add cmake-install target
- FUSE - Make utils.h usable under C

### Fixed
- Driver - Support hibernation mode 
- Driver- BSOD during unmounts -> Restore `DokanStopCheckThread`
- Mirror - Use `NTFS` as default file system name 
- Driver - Application using `SocketConnection` in dokan device 
- Library - `QueryServiceStatus` return value check
- FUSE - Fix directory deleting if opendir is not hooked
- Driver - Start failure on Win7 x86
- Mirror - Fix all issues reported by WinFSTest
- Driver - BSOD: network drive fileObject has no Vpb
- Library - Null-terminate string in unmount
- Library - Capitalization of `windows.h` include & `CMakeLists` file name
- FUSE - mount that was calling destroy ops
- Driver - `DokanGetFCB` was not case sensitive
- FUSE - Force getattr since Windows use readdir information compared to libfuse behavior
- FUSE - Call statfs with the root directory /
- FUSE - Wrong opt default value
- Driver - Save `DOKAN_DELETE_ON_CLOSE` in CCB and restore CCB flag during cleanup
- Mirror - Replace main return -1 by `EXIT_FAILURE`
- Mirror - Low and high param inversions for Un/LockFile
- Installer - Correctly check KB3033929 is installed on Win7
- Driver - Notify correctly when a file is removed with `FILE_FLAG_DELETE_ON_CLOSE`

### Removed
- Dokanctl - unused /f option during unmount
- Library - `SetLastError(ERROR_ALREADY_EXISTS);` in CreateFile logic by directly return `STATUS_OBJECT_NAME_COLLISION`

## [0.8.0] - 2015-12-10
### Added
- Installer - Embed VC++ redistributable in DokanInstall_X.X.X_redist.exe
- Alternative Streams enumeration support #48
- `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` and `IOCTL_STORAGE_GET_MEDIA_TYPES_EX` disk device `IOCTL` https://github.com/dokan-dev/dokany/commit/08b09a3910dbac0e902a81ad9e9ae1a06d4a6d90
- The timeout per device has been implemented. The DokanOptions has a new property Timeout (Milliseconds) #55 
- Installer - include PDB files
- Coverity
- FUSE - readonly flag https://github.com/dokan-dev/dokany/pull/90
- Write protect device option https://github.com/dokan-dev/dokany/pull/105
- Mounted DokanOperation is now called when device is mounted
- Library - Logs in DispatchQueryInformation and DokanPrintNTStatus

### Changed
- Installer - Move dokan include and lib files
- Driver - Central error handling #56
- Library - Return type Dokan API to NTSTATUS #65
- Installer - Move install-pdb to the main installer as an option
- Move to WDK 10 & Visual Studio 2015
- Move to clang-format code style LLVM
- `CreateFile` is moved to CreateFileW https://github.com/dokan-dev/dokany/pull/91
- Library - `CreateDirectory` & `OpenDirectory` have been merged with CreateFileW
- FUSE - Use struct stat from cygwin as struct `FUSE_STAT` for better compatibility https://github.com/dokan-dev/dokany/pull/88
- Dokanctl - Driver path is now resolved from %SystemRoot% https://github.com/dokan-dev/dokany/pull/104
- FUSE - Library is now a dynamic library (was static)
- Dokanctl - update showusage
- Library - Unmount has been renamed as Unmounted and are now called when device is unmounted #117
- Mirror - Ensure to have `SE_SECURITY_NAME` privilege and fix `GetFileSecurity` 
- FUSE - get_disk_free_space() return error changed #114

### Fixed
- Library - Use _malloca() for debug strings to avoid buffer overflows https://github.com/dokan-dev/dokany/pull/84
- Driver - `CreateFile` with empty FileName
- Driver - BSOD in security #55 
- Prevent thread termination if not enough resources #55
- Mirror - cannot delete empty directory #54
- Installer - Clean the register after uninstall
- Installer - Remove the driver after reboot
- Installer - Move dokan include and lib files  1f94c875bc90c339b1f7bb2e57dcbac514e0a6bc
- Library - `DokanMain` Deadlock with Network device #81 
- Library -  `DokanUnmount `failing to unmount a drive https://github.com/dokan-dev/dokany/pull/79
- Driver - Remove potential memory leak in `DokanCreateDiskDevice` on allocation failure https://github.com/dokan-dev/dokany/pull/108
- Miror - Wrong success when `CreateFile` was called to open directory for delete
- Library - `CreateFile` now correctly set last-error code to `ERROR_ALREADY_EXISTS` when open success with `CREATE_ALWAYS` and `OPEN_ALWAYS` (see mirror)

### Removed
- Library - Keep Alive option - Enabled as default
- Driver - Deadcode in `IRP_MJ_SHUTDOWN`


## [0.7.4] - 2015-08-21
### Added
- Fuse include to installer #37 

### Changed
- Doc updated #41 & Fix grammar #38 

### Fixed
- #26
- MoveFileEx #39

## [0.7.2] - 2015-05-12
### Added
- AppVeyor CI build

### Changed
- Code style standardization

### Fixed
- `DokanUnmount` driver letter name (#14)
- Bad Pool BSOD on DokanGetFCB function (#11 / https://code.google.com/p/dokan/issues/detail?id=229)
- DbgPrint message on `CreateDisposition` function (#13)

## [0.7.1] - 2015-02-04
### Fixed
- Dokan.lib exported functions name
- Missing `KeEnterCriticalRegion` function call in sys/fileinfo.c

### Removed
- Japan readme


## [0.7.0] - 2015-01-30
### Added
- Extended drive letters usage range from D - Z to B - Z (https://github.com/BenjaminKim/dokanx/pull/13)

### Changed
- Moved to WDK8.1 with Visual Studio 2013 support
- Improved DotNet wrapper with nullable datetime

### Fixed
- Fixed dokan_mirror offset processing on large files (https://github.com/BenjaminKim/dokanx/pull/18)
- Build warnings
- Dokan installer

### Removed
- PAGED_CODE declarations

## [0.6.0] - 2014-12-10
Latest Dokan version from Hiroki Asakawa.
 [http://dokan-dev.net/en]( http://web.archive.org/web/20150419082954/http://dokan-dev.net/en/)


[Unreleased]: https://github.com/dokan-dev/dokany/compare/v1.0.0...master
[1.0.0.5000]: https://github.com/dokan-dev/dokany/compare/v0.8.0...v1.0.0
[0.8.0]: https://github.com/dokan-dev/dokany/compare/v0.7.4...v0.8.0
[0.7.4]: https://github.com/dokan-dev/dokany/compare/0.7.2...v0.7.4
[0.7.2]: https://github.com/dokan-dev/dokany/compare/0.7.1...0.7.2
[0.7.1]: https://github.com/dokan-dev/dokany/compare/0.7.0...0.7.1
[0.7.0]: https://github.com/dokan-dev/dokany/compare/0.6.0...0.7.0
[0.6.0]: https://github.com/dokan-dev/dokany/tree/0.6.0
