# Change Log
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/) and this project adheres to [Semantic Versioning](http://semver.org/).

## [Unreleased] - 1.2.0.1000
### Added
- Build - Add ARM64

### Changed
- Installer - Remove .NET dependency.
- Build - Remove Windows 10 build for ARM
- Library - Allow usage driver letter `A`
- Documentation - Add `FSName` notice for `NTFS` & `FAT`
- Documentation - Add `GetFileSecurity` return `STATUS_NOT_IMPLEMENTED` remark
- Library - Update `DOKAN_VERSION` to 120 and `DOKAN_MINIMUM_COMPATIBLE_VERSION` to 110
- Kernel - Only set FO_FILE_MODIFIED for no paging io during write complete

### Fixed
- Library - Missing session id in `DOKAN_CONTROL` for user space
- NetworkProvider - UNC paths using only for current session show offline for other session.
- Installer - Dokan Network Provider - Move back `dokannp1.dll` to `system32` folder and `SysWow64`
- Mirror - Initialize `userTokenHandle` correctly
- FUSE -  Return correct status when file is open `FILE_OVERWRITE_IF` or `FILE_OPEN_IF` successfully
- Kernel - PageIO Dead lock
- Library - Get correct name (not uppercase) when repase point mount is used 

## [1.1.0.2000] - 2018-01-19
### Fixed
- Installer - Fix Wrong redist download link rename
- Installer - Fix vc++ version number displayed
- Installer - Update message download VC link

## [1.1.0.1000] - 2017-11-28
### Added
- Mirror - Add Impersonate Option for Security Enhancement.
- FUSE - Add read-only option
- Installer - Add VCRedistVersion variable / Now display version needed
- Dokanctl - Add usage option /?
- Kernel / Library - Add New FileRenameInformationEx since Windows 10 RS1

### Changed
- FUSE - cross-compile 32-bit Cygwin DLL from 64-bit
- Library - Merge DokanMapStandardToGenericAccess with DokanMapKernelToUserCreateFileFlags
- Move to VS 2017 / v141 / SDK 10.0.16299.0 / Installer Redist 2017

### Fixed
- Kernel - Fix current session unmount not releasing the device properly
- Mirror - Cannot open a read only file for delete 
- Mirror - Fix SetFileAttributes implementation by not updating when FileAttributes is 0x00
- Installer - Wrong new logo size 
- Kernel - Fixes #616 Only lock when not paging io 

## [1.0.5.1000] - 2017-09-19
### Added
- Kernel - Add `FILE_NOTIFY_CHANGE_SECURITY` during SetSecurity

### Changed
- Kernel - Createfile move `DOKAN_DELETE_ON_CLOSE` set flag after create success
- Kernel - Return access denied for paging file open request

### Fixed
- Kernel - CreateFile return `STATUS_DELETE_PENDING` for a request without share delete during a pending delete
- Mirror - `FindClose` is not being called if `GetLastError` returns anything other `ERROR_NO_MORE_FILES`

## [1.0.4.1000] - 2017-08-31
### Added
- Library - Support `FileIdFullDirectoryInformation`
- CI - IFSTest !
- Kernel - Add `FILE_NOTIFY_CHANGE_LAST_WRITE` in cleanup after write
- Kernel - Notify file size changed after a write beyond old size

### Changed
- Mirror -  Query underlying fs for filesystem flags and AND them with mirror default flags.
			Get filesystem name and maximum component length from underlying fs.
			Change default maximum component length from 256 to 255.
- Library - Doc Add context release info in CreateFile
- Build - PS Sign - Add env variables required in comments
- Mirror - Ensure the Security Descriptor length is set in mirror
- Library - `DokanNetworkProviderUninstall` Make a single call of wcsstr
- Library - `DokanNetworkProviderUninstall` if `DOKAN_NP_NAME` is already removed return `TRUE`
- Mirror - Return `STATUS_INVALID_PARAMETER` error when folder is asked to be created with `FILE_ATTRIBUTE_TEMPORARY`
- Mirror - Always set `FILE_SHARE_READ` for directory to avoid sharing violation for `FindFirstFile`
- Library - When looking parent folder if we have the right to remove a file, cleanup `FILE_NON_DIRECTORY_FILE`
- Library - Set proper information for `FILE_OVERWRITE` (`TRUNCATE_EXISTING`) 
- Mirror - Microsoft doc say `TRUNCATE_EXISTING` need GENERIC_WRITE so we add it

### Fixed
- Installer - Exe not signed
- Mirror - add `FILE_NAMED_STREAMS` to FileSystemFlags
- Kernel - Issue #490 #502 #503 #554 #412
- Library - Fix dokanctl UAC execution level
- FUSE - Warning due to `DWORD` printed as %d
- FUSE - Braces warning and remove commented code
- Kernel - BSOD with verifier enabled
- Kernel - BSOD during searching the backslash
- Kernel - Buffer len check `IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME`
- Kernel - Fix wrong error return for invalid relative file creation with leading slash
- Mirror - Return proper error when open a directory with `FILE_NON_DIRECTORY_FILE`
- Mirror - Cannot overwrite a hidden or system file if flag not set return `STATUS_ACCESS_DENIED`
- Mirror - Update FileAttributes with previous when `TRUNCATE_EXISTING` file 

## [1.0.3.1000] - 2017-03-24
### Added
- Installer - WiX: Ship PDB-files for `dokanfuse.dll`.
- Add Windows on ARM support.
- FUSE - Add uncname option
- Mirror - Add optional long path max
- Library - Add `DefaultGetFileSecurity` when `GetFileSecurity` is not handled by user mode

### Changed
- Library - Improve some mount error messages.
- FUSE - Return error when file open as directory with `FILE_NON_DIRECTORY_FILE`.
- Kernel - Clean all global disk device data in `CleanupGlobalDiskDevice`
- Kernel - Update mount point if mount manager did not follow our suggestion.

### Fixed
- Installer - Win10 x86 driver not properly signed for x86 anniversary.
- Kernel - Fix deadlock in `DokanDispatchCleanup`.
- Kernel - Do `MmFlushImageSection` if `ImageSectionObject` is set during cleanup.
- Library - Don't send free'd `DOKAN_OPEN_INFO` pointer to the driver.
- Library - Fix printf param for unsigned int.
- Kernel - Add `DokanFreeMdl` for read operation in `DokanCompleteRead`.
- Kernel - Fix crash issue cause by canceling the copy operation.
- FUSE - Replace wrong error type returned
- Library - Change rename fixed buffer to dynamic alloc
- Library - Rename return directly if `MoveFile` is not implemented
- Library - Low buffer handling correction on `QueryDirectory`
- Library - Fix wrong buffer size provided to the kernel in `DokanGetMountPointList`
- Kernel - Fix `dokanGlobal` wrongly zeroed and clean resource in `DokanUnload`
- Kernel - Add missing `IoDeleteDevice` when `IoCreateSymbolicLink` fail
- FUSE - Check for non-empty directories on delete #270.
- Library - Use `NT_SUCCESS` in `CreateFile`

## [1.0.2.1000] - 2017-01-20
### Added
- FUSE - Add libfuse-compatible pkg-config
- Mirror - Add `DOKAN_OPTION_FILELOCK_USER_MODE` option with `/f`

### Changed
- FUSE - Use pkg-config for building mirror
- Kernel - Many improvement allocation stack and heap
- Kernel - Enable `PAGED_CODE` for `DokanCheckShareAccess`
- Mirror - Return empty SACL if mirror doesn't have SeSecurityPrivilege
- Library - Use `DeleteMountPoint` for removing reparse point instead of `DeleteVolumeMountPoint`
- Library - Remove Redundant control flow jump 

### Fixed
- Driver - Less wide locking
- Kernel - Align security descriptor to 4-byte boundary in `DokanDispatchSetSecurity`
- Library - Fix dokan context leak when CreateFile fail 
- Kernel - Fix BSOD. When drive is started using n option and procmon is attached the rename of files in the root folder is not possible
- Kernel - Relative path rename
- Library - Write Set correctly the userland NtStatus

## [1.0.1.1000] - 2016-11-04
### Added
- Library - `DokanMapStandardToGenericAccess` - Convert `IRP_MJ_CREATE` DesiredAccess to generic rights.

### Changed
- Driver - Use atomic operations for FCB and CCB flags instead of locks.
- Update Windows SDK to 10.0.14393
- Library - Call now `DeleteFile` and `DeleteDirectory` with `DeleteOnClose` set at a delete request OR canceled.
- Driver -  Double check that the returned security descriptor is valid before returning success on QuerySecurity.
- Installer - Enable dev tools by default.
- Driver - Return `STATUS_FILE_LOCKED_WITH_ONLY_READERS` during `PreAcquireForSectionSynchronization` when locked only with readers.
- Mirror - Open handle when `GetFileInformation` requested after cleanup.
- Kernel - Remove FCB `Resource` and `MainResource`. Use FCB Header `Resource` instead allocated with LookasideList.

### Fixed
- Driver - `CcPurgeCacheSection` could cause deadlock when FCB was locked in the same time.
- Driver - Deadlock on related FCB.
- FUSE - Race condition in Dokan FUSE.
- Driver - BSOD issue related to filesystem mount on Windows 10 build 14936.
- Driver - Unlock FCB during `FsRtlOplockBreakH` to let other request Lock FCB.
- FUSE - Set correctly Authenticated Users rights (Explorer menu context).
- Mirror - Reject when trying to open a file as a directory.
- Driver - Return correct status for `FSCTL_FILESYSTEM_GET_STATISTICS` - Can now net share on Windows Server 2012 R2

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


[Unreleased]: https://github.com/dokan-dev/dokany/compare/v1.1.0.2000...master
[1.1.0.2000]: https://github.com/dokan-dev/dokany/compare/v1.1.0...v1.1.0.2000
[1.1.0.1000]: https://github.com/dokan-dev/dokany/compare/v1.0.5...v1.1.0
[1.0.5.1000]: https://github.com/dokan-dev/dokany/compare/v1.0.4...v1.0.5
[1.0.4.1000]: https://github.com/dokan-dev/dokany/compare/v1.0.3...v1.0.4
[1.0.3.1000]: https://github.com/dokan-dev/dokany/compare/v1.0.2...v1.0.3
[1.0.2.1000]: https://github.com/dokan-dev/dokany/compare/v1.0.1...v1.0.2
[1.0.1.1000]: https://github.com/dokan-dev/dokany/compare/v1.0.0...v1.0.1
[1.0.0.5000]: https://github.com/dokan-dev/dokany/compare/v0.8.0...v1.0.0
[0.8.0]: https://github.com/dokan-dev/dokany/compare/v0.7.4...v0.8.0
[0.7.4]: https://github.com/dokan-dev/dokany/compare/0.7.2...v0.7.4
[0.7.2]: https://github.com/dokan-dev/dokany/compare/0.7.1...0.7.2
[0.7.1]: https://github.com/dokan-dev/dokany/compare/0.7.0...0.7.1
[0.7.0]: https://github.com/dokan-dev/dokany/compare/0.6.0...0.7.0
[0.6.0]: https://github.com/dokan-dev/dokany/tree/0.6.0
