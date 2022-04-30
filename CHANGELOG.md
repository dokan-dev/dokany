# Change Log
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](http://keepachangelog.com/) and this project adheres to [Semantic Versioning](http://semver.org/).

## [2.0.4.1000] - 2021-04-30

### Changed
- Kernel - Remove possible `UNCName` prefix in `Filename` during `CreateFile`.
- Library - Doc - Recommend `FindFiles` to be implemented
- Library - Allow `FindFiles` to be optional by using `FindFilesWithPattern` with wildcard.

### Fixed
- Library - Move `UserContext` under `OpenCount` lock to reduce incorrect value used when calling `Close`.
- Kernel - Clear write change flag when FCB is being reused.
- Kernel - Remove unsupported eject media support (deadlock).
- FUSE - Update library name in module definition.

## [2.0.3.2000] - 2021-02-27

### Fixed
- Installer - Regenerate all `GUID` to fix uninstall and conflict with v1.

## [2.0.3.1000] - 2021-02-06

### Added
- Library - Add 16-128k `IoBatch/EventResult` pool for read and write.

### Changed
- MemFS - Replace `std::mutex` by `std::shared_mutex`.
- Library - Detect and use the best number of pulling threads with a minimum of two (unless single thread mode enabled) and a max of 16. Above 16, the io batch is enabled to use the extra threads to execute the possible extra request pulled.
- Library - Expensive `ZeroMemory` on large Read and Write buffers were removed.
- Library - Use absolute path for `NetworkProvider` registration `ProviderPath`.

### Fixed
- Installer - Change driver `GUID` to avoid installer to uninstall v1 files during the installation.
- Library - Fix incorrect lock hold when manipulating IoBatch.
- Library - Add lock to avoid `Unmounted` callback to be called multiple times (each pulling thread).
- MemFS - Fix double instance free on `CTRL+C`.

## [2.0.2.1000] - 2021-02-06

### Fixed
- Library - Fix `DokanResetTimeout` DokanFileInfo usage.
- Library - Correctly set PullEventTimeoutMs value.
- Kernel - Fix notify queue timeout value and timeout detection.
- Memfs - Fix `SetFileAttributes` behavior.
- Library - Fix incorrect lock used for DokanOpenInfo `OpenCount` increments.

### Changed
- Library - Use multiple main pull thread (4 instead of 1) to avoid deadlock during low activity.

## [2.0.1.2000] - 2021-01-28

### Fixed
- Installer - Set new BundleUpgradeCode for v2.
- Kernel - Lock Fcb during setFileInfo notify report change

## [2.0.1.1000] - 2021-01-23

### Added
- Kernel - Use the Rtl API to store DokanFCB in a Adelson-Velsky/Landis(AVL) table

### Fixed
- Kernel - Fix possible `PullEvents` infinite loop.
- Kernel - Fix crash when v2 and v1 are installed.
- Kernel - Make `RemoveSessionDevices` thread safe.
- Library - Fix `OpenRequestToken` crash when called outside `CreateFile`.
- Kernel - Fix subfolder property when using mount manager.

### Changed
- Installer - Allow installation side to side with 1.x.x.
- Library / Kernel - Enforce UNC usage with network drive option.
- Memfs - Update mount point in memfs after mount in case it changes.
- Library - Silence expected failures during unmount and make `DeleteDokanInstance` safer.
- Memfs - Use the new mount async API.
- Kernel - Release `NotifyEvent` memory on unmount.
- Kernel - Use IRP buffer fct for `GetMountPointList`.
- Kernel - Cleanup `SetInformation` completion and remove `DokanCCB::ERESOURCE`.

## [2.0.0.2000] - 2021-01-01

### Fixed
- Library - Missing new 2.0.0 API export.
- Library - Move `DOKAN_FILE_INFO.ProcessingContext` offset in struct to avoid padding issues.

## [2.0.0.1000] - 2021-12-30

See [here](https://github.com/dokan-dev/dokany/wiki/Update-Dokan-1.1.0-application-to-Dokany-2.0.0) how to migrate an existing > 1.1.0 filesystem to 2.0.0.

### Added
- Kernel / Library - Introduce Thread & Memory pool to process and pull events. This is highly based on #307 but without the async logic. The reason is to avoid using the kernel `NotificationLoop` that was dispatching requests to workers (userland thread) sequentially since wake up workers have a high cost of thread context switch.
The previous logic is nice when you want workers to be async (like #307) but as we have threads (and now even a thread poll) dedicated to pull and process events, there is no issue to make them synchronously wait in kernel for new events and directly take them from the pending request list.
The library will start with a single main thread that pulls events by batch and dispatches them to the thread pool but keeps the last one to be executed (or the only one) to be executed on the same thread. Each thread waken will do the same and pull new events at the same time. If none is returned, the thread goes back to sleep and otherwise does the same as the main thread (dispatch and process...etc). Only the main thread waits indefinitely for new events while others wait 100ms in the kernel before returning back to userland.Batching events, thread and memory pool offers a great flexibility of resources especially on heavy load.Thousands of lines of code were changed in the library (thanks again to @Corillian contribution of full rewrite) but the public API hasn't changed much.After running multiple benchmarks against `memfs`, sequential requests are about 10-35% faster but in the real world with the thread pool the perf are way above. @Corillian full rewrite of `FindFiles` actually improved an astonishing +100-250%...crazy.
- Library - `DokanCreateFileSystem` creates a filesystem like `DokanMain` but is async and will directly return when mount happens. `DokanWaitForFileSystemClosed` will wait until the filesystem is unmount and `DokanIsFileSystemRunning` can be used to check if it is still running. `DokanCloseHandle` will trigger an unmount and wait for it to be completed.
- Library - `DokanInit` and `DokanShutdown` are two new API that need to be called before creating filesystems and when dokan is no longer needed. They allocate internal mandatory dokan resources.  
- Kernel / Library - A Volume Security descriptor can now be assigned to `DOKAN_OPTIONS.VolumeSecurityDescriptor` to personalize the volume security permissions.
- Kernel - `FSCTL_EVENT_PROCESS_N_PULL` replace `IOCTL_EVENT_WAIT` and `IOCTL_EVENT_INFO` to process a possible answer and pull new events.
- Kernel - If Mount manager is enabled and the drive letter provided is busy, the drive will try to release the drive letter if it owns it. This can be useful during fast mount & unmount. If the drive letter is still busy after that, Mount manager is asked to assign a new one for us and will be provided to userland through `EVENT_DRIVER_INFO.ActualDriveLetter`.
- Library - `DOKAN_OPERATIONS.Mounted` now has a `MountPoint` param that will return the actual mount point used (see above on why it can be different).
- Library - `DOKAN_FILE_INFO.ProcessingContext` is a new Dokan reserved field currently used to pass information during a `FindFiles`.

### Changed
- Kernel - Major API has moved to version 2. This version is not compatible with dokan version 1.x.x.
- Kernel - Remove legacy Keepalive logic.
- Kernel - Remove legacy IOCTL.
- Kernel - Enable `DOKAN_EVENT_ENABLE_FCB_GC` by default and the interval can be set from userland (currently not public).
- Memfs - Unmount drive when `Ctrl + C` is used.
- Kernel - Move back `DOKAN_CONTROL` to the private kernel header to avoid sharing kernel variables to userland. Instead, `DOKAN_MOUNT_POINT_INFO` was created for this purpose and taken over in the different userland API that was using `DOKAN_CONTROL`.
- Library - `DOKAN_OPTION_*` values were reordered and reassigned.
- Library - `DOKAN_OPTIONS.ThreadCount` was replaced by `DOKAN_OPTIONS.SingleThread` since the library now uses a thread pool that allocats workers depending on workload and the available resources.
- Library - `PFillFindStreamData` now returns `FALSE` if the buffer is full, otherwise `TRUE`. It also requires to pass the `FindStreamContext` argument received during `FindStreams` instead of the previous `DokanFileInfo`.
- Library - `DokanGetMountPointList` and `DokanReleaseMountPointList` use the new `DOKAN_MOUNT_POINT_INFO` instead of the now private `DOKAN_CONTROL`.
- Library - All `DokanNotify*` functions now require the `DOKAN_HANDLE` created by `DokanCreateFileSystem`.
- Memfs - Pass already processed `stream_names` when adding the node.

### Fixed
- Library - `DOKAN_EVENT_DISPATCH_DRIVER_LOGS` is now usable to retrieve Kernel logs even on release build due to the batching events now enabled by default.

## [1.5.1.1000] - 2021-11-26
### Added
- Mirror - Add an option to personalize the volume name.

### Changed
- Installer - Add Debug drivers in cab for submission (Driver signature).
- Dokanctl - Remove non available unmount by id option.

### Fixed
- Memfs - Invalid create disposition log type.
- FUSE - Use `stbuf` in `readdir` callback.
- Kernel - Fix relative rename with Volume device as `RootDirectory` BSOD.
- Kernel - Return a mount failure and cleanup the devices when the driver fails to set the mount point folder reparse point.
- Kernel - Reset top-level IRP when creating / deleting reparse points.
- Kernel - Delete device on `InsertMountEntry` failure.

## [1.5.0.3000] - 2021-05-31
### Changed
- Installer - Add Win7 KB4474419 requirements (replace previous KB3033929).

### Fixed
- Kernel - Fix break exit type no longer having the wanted effect.

## [1.5.0.2000] - 2021-05-26
### Fixed
- Kernel - Incorrect eventLength usage for read/flush/security event after merge 981575c.

## [1.5.0.1000] - 2021-05-25
### Added
- Kernel - Add AllowIpcBatching option. [Looking for help to implement in the library.](https://github.com/dokan-dev/dokany/issues/981)
- Kernel - Allow kernel driver logs to be dispatched to userland.
- Kernel/Library - Add an option to use FSCTL Event type instead of IOCTL with dwDesiredAccess nullified.
- Kernel - Support Simple / Fully Qualified / Relative rename.

### Changed
- Kernel - Remove legacy -1 status value conversion.
- Kernel - Remove unused QueryDeviceRelations.
- Kernel - Convert `DokanLogInfo` to `DDbgPrint` temporarily in `DokanMountVolume` until we have a better logging solution.
- Kernel - Remove unused PNP IRP.
- Kernel/Library - Removing `DOKAN_EVENT_DISABLE_OPLOCKS` flag.
- Kernel - Centralize Irp Completion & Logging (Begin / End) and wrap request information into a `RequestContext`.
- Mirror - Use `GetDiskFreeSpaceEx` to support larger volume space.
- Kernel - Remove legacy mount service IOCTL code.
- FUSE - Return a valid fuse instance during `fuse_get_context`.
- Mirror - Enable long path support by default.
- Kernel - Fill `FILE_NAME_INFORMATION` in Kernel side during `FILE_ALL_INFORMATION`.
- Kernel - Remove unsupported `FileNetworkPhysicalNameInformation` QueryInformation.

### Fixed
- Library - Fix rename with double `\` for drive network shared.
- FUSE - Reuse `fuse_unmount` during `fuse_exit` to trigger `fuse_loop` to exit after Driver unmount the drive.
- Kernel - Fix a very rare race condition that make library fail to detect unmount.
- Kernel - Release CancelRoutine during Create timeout.
- Kernel - Fix invalid buffer size count when `PREPARE_OUTPUT` is used with types ending with a dynamic size field that are later filled with `AppendVarSizeOutputString`.

## [1.4.1.1000] - 2021-01-14
### Added
- Kernel/Library - Added support for `FileIdExtdDirectoryInformation`. Fixes directory listings under WSL2.
- Kernel/Library - Add `DOKAN_OPTION_CASE_SENSITIVE` mount option.
- Library - Add `DOKAN_OPTION_ENABLE_UNMOUNT_NETWORK_DRIVE` to allow unmounting network drive from explorer.
- FUSE - Add removable drive option and use local drive as default type now.

### Changed
- Library - C++ redistributable dependencies is fully removed for this release.
- Installer - Remove no longer needed dependency to KB2999226 (VC Redist).
- Kernel - Change `DOKAN_CONTROL.VolumeDeviceObject` to `ULONG64` for other compiler than MSVC.
- FUSE - Change default filesystem name to NTFS.

### Fixed
- Library - Reduce desired access rights for loop device handle and keepalive handle. Avoid some antivirus incompatibility.
- Library - No longer wait for apps to answer `BroadcastSystemMessage` during mount.
- Library - Return `STATUS_INVALID_PARAMETER` where appropriate. Fixes directory listings under WSL2.
- FUSE - Incorrect convertion for MountPoint using chinese characters.
- MemFS - Fix out of range read when the offset is bigger than the buffer.
- MemFS - Always remove `FILE_ATTRIBUTE_NORMAL` as we set `FILE_ATTRIBUTE_ARCHIVE` before.
- MemFS - Correctly handle current session option

## [1.4.0.1000] - 2020-01-06
### Added
- MemFS - Add a new FS sample project: dokan_memfs. MemFS is a better example to debug and know the dokan driver/library feature supported and NTFS compliant. The FS pass most of WinFSTest and IFSTest. It looks to be stable enough to be included in the installer. It hasn't been test with real usage but it is expected to run without issue. MemFS is written in c++ and is under MIT license.
- Installer - You can now find two installers with binaries built with and without VC redistributable. If no issue is reported, the next release will only have without the VC redistributable.
- Kernel - Support directory path mount with mount manager. 
- Kernel - Add `DokanAllocZero` that Alloc and ZeroMemory buffer size requested sys - Rename `ExAllocatePool` to `DokanAlloc`.
- Kernel - Add Input IRP Buffer Help macro.
- Kernel - Use `GET_IRP` for `Type3InputBuffer` and start using Output buffer function helpers for IRP.
- FUSE - Add mount manager option.

### Changed
- Kernel - Replace `DOKAN_OPTION_OPTIMIZE_SINGLE_NAME_SEARCH` by `DOKAN_OPTION_ENABLE_FCB_GARBAGE_COLLECTION`. The advantage of the GC approach is that it prevents filter drivers from exponentially slowing down procedures like zip file extraction due to repeatedly rebuilding state that they attach to the FCB header.
- Kernel - Multiple code refactoring.
- Kernel - Move and Improve `FixFileNameForReparseMountPoint`.
- Kernel - `FileNetworkPhysicalNameInformation` now return directly the FileName instead of sending the request to userland.
- Kernel - `FileAllocationInformation` - return `STATUS_USER_MAPPED`_FILE when trying to truncate a memory mapped file.
- Kernel - Do not rethrow exception during `EXCEPTION_EXECUTE_HANDLER`.
- Kernel - During EventRelease directly call `FsRtlNotifyCleanupAll` instead of going through all Fcb & Ccb.
- Kernel - Change `DokanPrintNTStatus` with limited number of `NTSTATUS` values print to use `DokanGetNTSTATUSStr`. `DokanGetNTSTATUSStr` use `ntstatus_log.inc` that has all ntstatus from <ntstatus.h>.
- Library - Add support for compiling with GCC.
- Library - Move `DokanRemoveMountPointEx` to internal header as it is not available to the public API.

### Fixed
- FUSE - Read keep local filename instance.
- Installer - Fix incorrect pdb file copied for driver.
- Library - `DokanNetworkProviderInstall` return earlier if a Reg failure happens and not corrupt the registry.
- Kernel - Catch `FsRtlNotifyFullReportChange` `STATUS_ACCESS_VIOLATION` exception.
- Library - Correct newName format when a stream name is renamed. 

## [1.3.1.1000] - 2019-12-16
### Added
- Kernel - Added support for `FileIdExtdBothDirectoryInformation`, which is required when the target is mapped as a volume into docker containers.

### Changed
- Kernel - Single build target Win7 / enable new features according to OS during runtime. Remove supported XP/Vista code.
- Kernel - Only log to event viewer when debug default log is enabled.
- Library - Clarified documentation of dokan file-change notification functions.
- Build - Run Code Analysis on all builds of debug build configurations within Visual Studio, but not by default from msbuild.
- Mirror - Add `GetDiskFreeSpace` UNC as Root support.

### Fixed
- Library - Incorrect call to `legacyKeepAliveThreadIds` `WaitForObject`.
- Kernel - `FileNameInformation` - Only concat `UNCName` / `DiskDeviceName` for network devices.
- FUSE - Infinite loop when using characters from Unicode supplementary planes ('🔈' for example).
- FUSE - Support `WriteFile` with offset `-1`.
- FUSE - `get_file` - Do not use the current file shared mode.

## [1.3.0.1000] - 2019-07-24
### Added
- Mirror - Use `GetDiskFreeSpace` during `MirrorDokanGetDiskFreeSpace`.
- Kernel - Add most important log msg to Event Viewer.
- Kernel - Add `DOKAN_OPTION_DISABLE_OPLOCKS` dokan option.
- Kernel - Add check that `DeviceControl` are performed on a volume handle and not a file.
- Kernel - Add `DOKAN_OPTION_OPTIMIZE_SINGLE_NAME_SEARCH` dokan option to speedup Win7 file name normalization.
- Library - Add functions to notify Dokan Kernel that files in use fs has changed `DokanNotifyCreate / DokanNotifyDelete / DokanNotifyUpdate / DokanNotifyXAttrUpdate / DokanNotifyRename`.
- SetAssemblyVersion - Now update `DOKAN_MAJOR_API_VERSION`.
- Kernel - Write - Check total event length is not longer than what we can allocate.

### Changed
- Use latest WDK & SDK for Windows 10 version 1903 and toolset v142.
- Installer - Update VCRedistVersion to VS 2019 14.21.27702.
- Mirror - Improve ShowUsage.
- Library - `DokanGetMountPointList` now returns his own buffer that need to be released with `DokanReleaseMountPointList`.
- Kernel - Return proper error status for `DFileSystemControl`.
- Kernel - Fix OpLocks / Remove FCB Retry lock.
- Kernel - Use debug mode option to enable lock or/and oplock kernel log.
- Kernel - Rename `DOKAN_KEEPALIVE_TIMEOUT` to `DOKAN_KEEPALIVE_TIMEOUT_DEFAULT`

### Fixed
- Kernel - Fix long rename BSOD with network option enabled.
- Kernel - Fix root rename with 360 antivirus.
- Library - Use `DbgPrintW` instead of `DbgPrint` when printing wide characters.
- Library - Add error check for `_vscprintf` and `vsprintf_s` in `DokanDbgPrint`, and `_vscwprintf` and `vswprintf_s` in `DokanDbgPrintW`.
- Library - Fix `DokanUnmount` possible oob memory.
- Mirror - Fix possible oob memory during long findfiles path.
- Mirror - Fix possible oob memory during long DeleteDirectory path.
- Kernel - Lock global resources during `DokanGetMountPointList` avoid possible BSOD.
- Kernel - Send correct notify change during `FileRenameInformation` when move to a diff folder.
- Kernel - Move all `Io ShareAccess` under fcb RW lock.
- Dokannp - Add leading `\` to `UNCName` during `NPGetConnection`.

## [1.2.2.1000] - 2019-03-08
### Added
- FUSE - Expose allocation unit size and sector size.
- Kernel - Add new `FileDispositionInformationEx` for Windows 10, version 1709

### Changed
- Library - Increase `DOKAN_MAX_THREAD` from 15 to 63 for better performance.
- Kernel - `FileStreamInformation` now return directly `STATUS_NOT_IMPLEMENTED` if `UseAltStream` is disabled.
- Library - Improve `DokanIsNameInExpression` documentation

### Fixed
- Kernel - Wrong `szMountPoint->length` usage in `DokanGlobalEventRelease`
- Kernel - Fix handle KeepAlive calls before device fully started 

## [1.2.1.2000] - 2018-12-21
### Added
- SetAssemblyVersion - Add update dokan version define

### Fixed
- Library - Bump Dokan version to 121

## [1.2.1.1000] - 2018-12-18
### Changed
- Kernel/Library - Replace keepalive ping event by a single keep alive file handle
- Cert - Runs with admin rights and checks Secureboot is enabled

### Fixed
- Kernel - Fix Buffer Overflow by adding mount length path check 

## [1.2.0.1000] - 2018-08-09
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


[Unreleased]: https://github.com/dokan-dev/dokany/compare/v2.0.3.2000...HEAD
[2.0.3.2000]: https://github.com/dokan-dev/dokany/compare/v2.0.3.1000...v2.0.3.2000
[2.0.3.1000]: https://github.com/dokan-dev/dokany/compare/v2.0.2.1000...v2.0.3.1000
[2.0.2.1000]: https://github.com/dokan-dev/dokany/compare/v2.0.1.2000...v2.0.2.1000
[2.0.1.2000]: https://github.com/dokan-dev/dokany/compare/v2.0.1.1000...v2.0.1.2000
[2.0.1.1000]: https://github.com/dokan-dev/dokany/compare/v2.0.0.2000...v2.0.1.1000
[2.0.0.2000]: https://github.com/dokan-dev/dokany/compare/v2.0.0.1000...v2.0.0.2000
[2.0.0.1000]: https://github.com/dokan-dev/dokany/compare/v1.5.1.1000...v2.0.0.1000
[1.5.1.1000]: https://github.com/dokan-dev/dokany/compare/v1.5.0.3000...v1.5.1.1000
[1.5.0.3000]: https://github.com/dokan-dev/dokany/compare/v1.5.0.2000...v1.5.0.3000
[1.5.0.2000]: https://github.com/dokan-dev/dokany/compare/v1.5.0.1000...v1.5.0.2000
[1.5.0.1000]: https://github.com/dokan-dev/dokany/compare/v1.4.1.1000...v1.5.0.1000
[1.4.1.1000]: https://github.com/dokan-dev/dokany/compare/v1.4.0.1000...v1.4.1.1000
[1.4.0.1000]: https://github.com/dokan-dev/dokany/compare/v1.3.1.1000...v1.4.0.1000
[1.3.1.1000]: https://github.com/dokan-dev/dokany/compare/v1.3.0.1000...v1.3.1.1000
[1.3.0.1000]: https://github.com/dokan-dev/dokany/compare/v1.2.2.1000...v1.3.0.1000
[1.2.2.1000]: https://github.com/dokan-dev/dokany/compare/v1.2.1.2000...v1.2.2.1000
[1.2.1.2000]: https://github.com/dokan-dev/dokany/compare/v1.2.1.1000...v1.2.1.2000
[1.2.1.1000]: https://github.com/dokan-dev/dokany/compare/v1.2.0.1000...v1.2.1.1000
[1.2.0.1000]: https://github.com/dokan-dev/dokany/compare/v1.1.0.2000...v1.2.0.1000
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
