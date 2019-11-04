#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <sddl.h>
#include "utils.h"
#include "fusemain.h"
#include "dokanfuse.h"
#include "../../dokan/dokani.h"
#include <stdio.h>

#if defined(__GNUC__)
#define FPRINTF(f, args...)                                                   \
  do {                                                                         \
    fprintf(f, args);                                                         \
    fflush(f);                                                                 \
  } while (0)
#else
#define FPRINTF fprintf
#endif

#define the_impl                                                               \
  reinterpret_cast<impl_fuse_context *>(                                       \
      EventInfo->DokanFileInfo->DokanOptions->GlobalContext)

HINSTANCE hFuseDllInstance;

extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason,
                               LPVOID lpReserved) {
  if (dwReason == DLL_PROCESS_ATTACH) {
    hFuseDllInstance = hInstance;
    DisableThreadLibraryCalls(hInstance);
  }
  return TRUE;
}

static int WalkDirectoryWithSetFuseContext(DOKAN_FIND_FILES_EVENT *EventInfo, void *buf, const char *name,
	const struct FUSE_STAT *stbuf,
	FUSE_OFF_T off)
{
	impl_fuse_context *impl = the_impl;
	if (impl->debug())
		FPRINTF(stderr, "WalkDirectoryWithSetFuseContext on thread %d\n", GetCurrentThreadId());

	impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);
	return impl->walk_directory(buf, name, stbuf, off);

}

static NTSTATUS DOKAN_CALLBACK
FuseFindFiles(DOKAN_FIND_FILES_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
    FPRINTF(stderr, "FindFiles: %ls\n", EventInfo->PathName);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  return errno_to_ntstatus_error(
      impl->find_files(EventInfo, &WalkDirectoryWithSetFuseContext));
}

static void DOKAN_CALLBACK FuseCleanup(DOKAN_CLEANUP_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
	  FPRINTF(stderr, "Cleanup: %ls\n\n", EventInfo->FileName);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  impl->cleanup(EventInfo->FileName, EventInfo->DokanFileInfo);
}

static NTSTATUS DOKAN_CALLBACK
FuseCanDeleteDirectory(DOKAN_CAN_DELETE_FILE_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
	  FPRINTF(stderr, "DeleteDirectory: %ls\n", EventInfo->FileName);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  return errno_to_ntstatus_error(
      impl->delete_directory(EventInfo->FileName, EventInfo->DokanFileInfo));
}

struct Constant {
  DWORD value;
  const char *name;
};

#define CONST_START(name) Constant name[] = {
#define CONST_VAL(val) {val, #val},
#define CONST_END(name)                                                        \
  { 0, NULL }                                                                  \
  }                                                                            \
  ;

CONST_START(cAccessMode)
CONST_VAL(GENERIC_READ)
CONST_VAL(GENERIC_WRITE)
CONST_VAL(GENERIC_ALL)
CONST_VAL(GENERIC_EXECUTE)
CONST_VAL(DELETE)
CONST_VAL(READ_CONTROL)
CONST_VAL(WRITE_DAC)
CONST_VAL(WRITE_OWNER)
CONST_VAL(SYNCHRONIZE)
CONST_VAL(FILE_GENERIC_EXECUTE)
CONST_VAL(FILE_GENERIC_READ)
CONST_VAL(FILE_GENERIC_WRITE)
CONST_VAL(FILE_EXECUTE)
CONST_VAL(FILE_READ_ATTRIBUTES)
CONST_VAL(STANDARD_RIGHTS_EXECUTE)
CONST_VAL(FILE_READ_ATTRIBUTES)
CONST_VAL(FILE_READ_DATA)
CONST_VAL(FILE_READ_EA)
CONST_VAL(STANDARD_RIGHTS_READ)
CONST_VAL(FILE_APPEND_DATA)
CONST_VAL(FILE_WRITE_ATTRIBUTES)
CONST_VAL(FILE_WRITE_DATA)
CONST_VAL(FILE_WRITE_EA)
CONST_VAL(STANDARD_RIGHTS_WRITE)
CONST_VAL(FILE_ADD_FILE)
CONST_VAL(FILE_ADD_SUBDIRECTORY)
CONST_VAL(FILE_ALL_ACCESS)
CONST_VAL(FILE_APPEND_DATA)
CONST_VAL(FILE_CREATE_PIPE_INSTANCE)
CONST_VAL(FILE_DELETE_CHILD)
CONST_VAL(FILE_LIST_DIRECTORY)
CONST_VAL(FILE_TRAVERSE)
CONST_END(cAccessMode)

CONST_START(cShareMode)
CONST_VAL(FILE_SHARE_DELETE)
CONST_VAL(FILE_SHARE_READ)
CONST_VAL(FILE_SHARE_WRITE)
CONST_END(cShareMode)

CONST_START(cDisposition)
CONST_VAL(FILE_SUPERSEDE)
CONST_VAL(FILE_CREATE)
CONST_VAL(FILE_OPEN)
CONST_VAL(FILE_OPEN_IF)
CONST_VAL(FILE_OVERWRITE)
CONST_VAL(FILE_OVERWRITE_IF)
CONST_END(cDisposition)

void DebugConstant(const char *name, ULONG value, Constant *c) {
  while (c->name != nullptr && c->value != value)
    ++c;
  fprintf(stderr, "%s: %s (" PRIxULONG ")\n", name, c->name ? c->name : "unknown!",
          value);
}

void DebugConstantBit(const char *name, DWORD value, Constant *cs) {
  // check sorted and sort
  for (Constant *c = cs; c[1].name;) {
    if (c[0].value < c[1].value) {
      std::swap(c[0], c[1]);
      c = cs;
      continue;
    }
    ++c;
  }

  DWORD left = value;
  bool started = false;
  const char *sep = "";
  fprintf(stderr, "%s: ", name);
  for (Constant *c = cs; c->name; ++c) {
    if ((value & c->value) == c->value && (left & c->value) != 0) {
      fprintf(stderr, "%s%s", sep, c->name);
      sep = "|";
      left &= ~c->value;
      started = true;
    }
  }
  if (left || !started)
    fprintf(stderr, "%s0x%lX", sep, static_cast<long unsigned>(left));
  fprintf(stderr, "\n");
}

static NTSTATUS DOKAN_CALLBACK
FuseCreateFile(DOKAN_CREATE_FILE_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug()) {
    FPRINTF(stderr, "CreateFile: %ls\n", EventInfo->FileName);
    DebugConstantBit("\tDesiredAccess", EventInfo->DesiredAccess, cAccessMode);
    DebugConstantBit("\tShareAccess", EventInfo->ShareAccess, cShareMode);
    DebugConstant("\tDisposition", EventInfo->CreateDisposition, cDisposition);
    FPRINTF(stderr, "\tAttributes: " PRIuULONG " (0x" PRIxULONG ")\n", EventInfo->FileAttributes,
		EventInfo->FileAttributes);
    FPRINTF(stderr, "\tOptions: " PRIuULONG " (0x" PRIxULONG ")\n", EventInfo->CreateOptions, EventInfo->CreateOptions);

    fflush(stderr);
  }

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  if ((EventInfo->CreateOptions & FILE_DIRECTORY_FILE) == FILE_DIRECTORY_FILE) {

    if (EventInfo->CreateDisposition == FILE_CREATE || EventInfo->CreateDisposition == FILE_OPEN_IF) {
      return errno_to_ntstatus_error(
          impl->create_directory(EventInfo->FileName, EventInfo->DokanFileInfo));
    } else if (EventInfo->CreateDisposition == FILE_OPEN) {

      return errno_to_ntstatus_error(
          impl->open_directory(EventInfo->FileName, EventInfo->DokanFileInfo));
    }
  }

  return impl->create_file(EventInfo->FileName, EventInfo->DesiredAccess, EventInfo->ShareAccess,
	  EventInfo->CreateDisposition, EventInfo->FileAttributes, EventInfo->CreateOptions, EventInfo->DokanFileInfo);
}

static void DOKAN_CALLBACK FuseCloseFile(DOKAN_CLOSE_FILE_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "Close: %ls\n\n", EventInfo->FileName);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);
  impl->close_file(EventInfo->FileName, EventInfo->DokanFileInfo);
}

static NTSTATUS DOKAN_CALLBACK FuseReadFile(DOKAN_READ_FILE_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "ReadFile: %ls from %lld len %u\n", EventInfo->FileName,
		static_cast<__int64>(EventInfo->Offset), static_cast<unsigned>(EventInfo->NumberOfBytesToRead));

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  return errno_to_ntstatus_error(impl->read_file(
	  EventInfo->FileName,
	  EventInfo->Buffer,
	  EventInfo->NumberOfBytesToRead,
	  &EventInfo->NumberOfBytesRead,
	  EventInfo->Offset,
	  EventInfo->DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseWriteFile(DOKAN_WRITE_FILE_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;
  if (impl->debug()) {
    FPRINTF(stderr, "WriteFile: %ls, offset %lld, length " PRIuDWORD "\n", EventInfo->FileName,
		EventInfo->Offset, EventInfo->NumberOfBytesToWrite);
  }

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  return errno_to_ntstatus_error(
      impl->write_file(EventInfo->FileName,
		  EventInfo->Buffer,
		  EventInfo->NumberOfBytesToWrite,
		  &EventInfo->NumberOfBytesWritten,
		  EventInfo->Offset,
		  EventInfo->DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK
FuseFlushFileBuffers(DOKAN_FLUSH_BUFFERS_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
    FPRINTF(stderr, "FlushFileBuffers: %ls\n", EventInfo->FileName);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  return errno_to_ntstatus_error(
      impl->flush_file_buffers(EventInfo->FileName, EventInfo->DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseGetFileInformation(DOKAN_GET_FILE_INFO_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
    FPRINTF(stderr, "GetFileInfo: : %ls\n", EventInfo->FileName);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  return errno_to_ntstatus_error(impl->get_file_information(
	  EventInfo->FileName,
	  &EventInfo->FileHandleInfo,
	  EventInfo->DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseCanDeleteFile(DOKAN_CAN_DELETE_FILE_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
    FPRINTF(stderr, "DeleteFile: %ls\n", EventInfo->FileName);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  return errno_to_ntstatus_error(impl->delete_file(EventInfo->FileName, EventInfo->DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK
FuseMoveFile(DOKAN_MOVE_FILE_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "MoveFile: %ls -> %ls\n\n", EventInfo->FileName, EventInfo->NewFileName);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  return errno_to_ntstatus_error(
      impl->move_file(EventInfo->FileName,
		  EventInfo->NewFileName,
		  EventInfo->ReplaceIfExists,
		  EventInfo->DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseLockFile(DOKAN_LOCK_FILE_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
    FPRINTF(stderr, "LockFile: %ls\n", EventInfo->FileName);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  return errno_to_ntstatus_error(
      impl->lock_file(EventInfo->FileName,
		  EventInfo->ByteOffset,
		  EventInfo->Length,
		  EventInfo->DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseUnlockFile(DOKAN_UNLOCK_FILE_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
    FPRINTF(stderr, "UnlockFile: %ls\n", EventInfo->FileName);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  return errno_to_ntstatus_error(
      impl->unlock_file(EventInfo->FileName,
		  EventInfo->ByteOffset,
		  EventInfo->Length,
		  EventInfo->DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseSetEndOfFile(DOKAN_SET_EOF_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
    FPRINTF(stderr, "SetEndOfFile: %ls, %lld\n", EventInfo->FileName, EventInfo->Length);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(
      impl->set_end_of_file(EventInfo->FileName, EventInfo->Length, EventInfo->DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseSetAllocationSize(DOKAN_SET_ALLOCATION_SIZE_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
    FPRINTF(stderr, "SetAllocationSize: %ls, %lld\n", EventInfo->FileName, EventInfo->Length);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  BY_HANDLE_FILE_INFORMATION byHandleFileInfo;
  ZeroMemory(&byHandleFileInfo, sizeof(BY_HANDLE_FILE_INFORMATION));

  NTSTATUS ret = errno_to_ntstatus_error(
      impl->get_file_information(EventInfo->FileName, &byHandleFileInfo, EventInfo->DokanFileInfo));

  LARGE_INTEGER fileSize;
  fileSize.LowPart = byHandleFileInfo.nFileSizeLow;
  fileSize.HighPart = byHandleFileInfo.nFileSizeHigh;

  if (ret != 0) {
    return ret;
  }
  else if (EventInfo->Length < fileSize.QuadPart) {
    /* https://msdn.microsoft.com/en-us/library/windows/hardware/ff540232(v=vs.85).aspx
    * The end-of-file position must always be less than or equal to the
    * allocation size. If the allocation size is set to a value that is
    * less than the end - of - file position, the end - of - file position
    * is automatically adjusted to match the allocation size.*/
    return errno_to_ntstatus_error(
        impl->set_end_of_file(EventInfo->FileName, EventInfo->Length, EventInfo->DokanFileInfo));
  }
  else {
    return 0;
  }
}

static NTSTATUS DOKAN_CALLBACK FuseSetFileBasicInformation(DOKAN_SET_FILE_BASIC_INFO_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
    FPRINTF(stderr, "SetFileBasicInformation: %ls\n", EventInfo->FileName);

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  NTSTATUS result = errno_to_ntstatus_error(
      impl->set_file_attributes(EventInfo->FileName, EventInfo->Info->FileAttributes, EventInfo->DokanFileInfo));

  if(result == STATUS_SUCCESS)
  {
	  result = errno_to_ntstatus_error(impl->set_file_time(
		  EventInfo->FileName,
		  (const FILETIME*)&EventInfo->Info->CreationTime,
		  (const FILETIME*)&EventInfo->Info->LastAccessTime,
		  (const FILETIME*)&EventInfo->Info->LastWriteTime,
		  EventInfo->DokanFileInfo));
  }

  return result;
}

static NTSTATUS DOKAN_CALLBACK FuseGetVolumeFreeSpace(DOKAN_GET_DISK_FREE_SPACE_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;
  
  if (impl->debug())
    FPRINTF(stderr, "GetVolumeFreeSpace\n");

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  return errno_to_ntstatus_error(
      impl->get_disk_free_space(
		  &EventInfo->FreeBytesAvailable,
		  &EventInfo->TotalNumberOfBytes,
		  &EventInfo->TotalNumberOfFreeBytes,
		  EventInfo->DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK
FuseGetVolumeInformation(DOKAN_GET_VOLUME_INFO_EVENT *EventInfo) {

  impl_fuse_context *impl = the_impl;

  if (impl->debug())
    FPRINTF(stderr, "GetVolumeInformation\n");

  impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

  EventInfo->VolumeInfo->SupportsObjects = FALSE;
  EventInfo->VolumeInfo->VolumeSerialNumber = 0;

  DWORD fileSystemFlags = 0;
  WCHAR fileSystemName[64];

  NTSTATUS result = errno_to_ntstatus_error(impl->get_volume_information(
	  EventInfo->VolumeInfo->VolumeLabel,
	  EventInfo->MaxLabelLengthInChars,
	  fileSystemName,
      (DWORD)ARRAYSIZE(fileSystemName),
	  EventInfo->DokanFileInfo,
	  &fileSystemFlags));

  if(result == STATUS_SUCCESS)
  {
	  EventInfo->VolumeInfo->VolumeLabelLength = (ULONG)wcslen(EventInfo->VolumeInfo->VolumeLabel);
  }

  return result;
}

static NTSTATUS DOKAN_CALLBACK
FuseGetVolumeAttributes(DOKAN_GET_VOLUME_ATTRIBUTES_EVENT *EventInfo) {

	impl_fuse_context *impl = the_impl;

	if(impl->debug())
		FPRINTF(stderr, "GetVolumeAttributes\n");

	impl_chain_guard guard(impl, EventInfo->DokanFileInfo->ProcessId);

	EventInfo->Attributes->MaximumComponentNameLength = 255;

	WCHAR volumeName[256];
	
	NTSTATUS result = errno_to_ntstatus_error(impl->get_volume_information(
		volumeName,
		(DWORD)ARRAYSIZE(volumeName),
		EventInfo->Attributes->FileSystemName,
		EventInfo->MaxFileSystemNameLengthInChars,
		EventInfo->DokanFileInfo,
		&EventInfo->Attributes->FileSystemAttributes));

	if(result == STATUS_SUCCESS)
	{
		EventInfo->Attributes->FileSystemNameLength = (ULONG)wcslen(EventInfo->Attributes->FileSystemName);
	}

	return result;
}

static void DOKAN_CALLBACK FuseMounted(DOKAN_MOUNTED_INFO *EventInfo) {

  impl_fuse_context *impl = reinterpret_cast<impl_fuse_context *>(EventInfo->DokanOptions->GlobalContext);

  if (impl->debug())
    FPRINTF(stderr, "Mounted\n");

  impl_chain_guard guard(impl, 0);

  impl->mounted(EventInfo);
}

static void DOKAN_CALLBACK FuseUnmounted(DOKAN_UNMOUNTED_INFO *EventInfo) {
  
	impl_fuse_context *impl = reinterpret_cast<impl_fuse_context *>(EventInfo->DokanOptions->GlobalContext);

  if (impl->debug())
    FPRINTF(stderr, "Unmount\n");

  impl_chain_guard guard(impl, 0);

  impl->unmounted(EventInfo);
}

int fuse_interrupted(void) {
  return 0; // TODO: fix this
}

static DOKAN_OPERATIONS dokanOperations = {
    FuseCreateFile,
    FuseCleanup,
    FuseCloseFile,
    FuseReadFile,
    FuseWriteFile,
    FuseFlushFileBuffers,
    FuseGetFileInformation,
    FuseFindFiles,
	nullptr, // FindFilesWithPattern
    FuseSetFileBasicInformation,
    FuseCanDeleteFile,
    FuseMoveFile,
    FuseSetEndOfFile,
    FuseSetAllocationSize,
    FuseLockFile,
    FuseUnlockFile,
	FuseGetVolumeFreeSpace,
    FuseGetVolumeInformation,
	FuseGetVolumeAttributes,
    FuseMounted,
    FuseUnmounted,
	nullptr,
	nullptr, // SetFileSecurity
	nullptr, // FindStreams
};

int do_fuse_loop(struct fuse *fs, bool mt) {
  if (!fs->ch.get() || fs->ch->mountpoint.empty())
    return -1;

  // Calculate umasks
  int umask = fs->conf.umask;
  if (umask == 0)
    umask = 0777; // It's OCTAL! Really!
  int dirumask = fs->conf.dirumask;
  if (dirumask == 0)
    dirumask = umask;
  int fileumask = fs->conf.fileumask;
  if (fileumask == 0)
    fileumask = umask;

  impl_fuse_context impl(&fs->ops, fs->user_data, fs->conf.debug != 0,
                         fileumask, dirumask, fs->conf.fsname,
                         fs->conf.volname, fs->conf.uncname);

  // Parse Dokan options
  PDOKAN_OPTIONS dokanOptions = static_cast<PDOKAN_OPTIONS>(malloc(sizeof(DOKAN_OPTIONS)));
  if (dokanOptions == nullptr) {
    return -1;
  }
  ZeroMemory(dokanOptions, sizeof(DOKAN_OPTIONS));
  dokanOptions->Options |=
      fs->conf.networkDrive ? DOKAN_OPTION_NETWORK : DOKAN_OPTION_REMOVABLE;
  dokanOptions->GlobalContext = reinterpret_cast<ULONG64>(&impl);

  wchar_t uncName[MAX_PATH + 1];
  if (fs->conf.networkDrive && fs->conf.uncname) {
    mbstowcs(uncName, fs->conf.uncname, MAX_PATH);
    dokanOptions->UNCName = uncName;
  }

  wchar_t mount[MAX_PATH + 1];
  mbstowcs(mount, fs->ch->mountpoint.c_str(), MAX_PATH);

  dokanOptions->Version = DOKAN_VERSION;
  dokanOptions->MountPoint = mount;
  dokanOptions->ThreadCount = mt ? FUSE_THREAD_COUNT : 1;
  dokanOptions->Timeout = fs->conf.timeoutInSec * 1000;
  dokanOptions->AllocationUnitSize = fs->conf.allocationUnitSize;
  dokanOptions->SectorSize = fs->conf.sectorSize;

  // Debug
  if (fs->conf.debug)
    dokanOptions->Options |= DOKAN_OPTION_DEBUG | DOKAN_OPTION_STDERR;

  // Read only
  if (fs->conf.readonly)
    dokanOptions->Options |= DOKAN_OPTION_WRITE_PROTECT;

  // Load Dokan DLL
  if (!fs->ch->init()) {
    free(dokanOptions);
    return -1; // Couldn't load DLL. TODO: UGLY!!
  }

  // The main loop!
  fs->within_loop = true;
  int res = fs->ch->ResolvedDokanMain(dokanOptions, &dokanOperations);
  fs->within_loop = false;
  return res;
}

bool fuse_chan::init() {
  dokanDll = LoadLibraryW(DOKAN_DLL);
  if (!dokanDll)
    return false;

  // check version
  typedef ULONG(__stdcall * DokanVersionType)();
  typedef void(__stdcall *DokanInitType)(DOKAN_MEMORY_CALLBACKS *memoryCallbacks, DOKAN_LOG_CALLBACKS *logCallbacks);

  DokanVersionType ResolvedDokanVersion;
  DokanInitType ResolvedInit;

  ResolvedInit = (DokanInitType)GetProcAddress(dokanDll, "DokanInit");
  ResolvedShutdown = (DokanShutdownType)GetProcAddress(dokanDll, "DokanShutdown");

  if(!ResolvedInit || !ResolvedShutdown) {

	  return false;
  }

  ResolvedInit(NULL, NULL);

  ResolvedDokanVersion =
      (DokanVersionType)GetProcAddress(dokanDll, "DokanVersion");

  if(!ResolvedDokanVersion || ResolvedDokanVersion() < DOKAN_VERSION) {

	  ResolvedShutdown();
	  ResolvedShutdown = NULL;

	  return false;
  }

  ResolvedDokanMain = (DokanMainType)GetProcAddress(dokanDll, "DokanMain");

  ResolvedDokanUnmount =
      (DokanUnmountType)GetProcAddress(dokanDll, "DokanUnmount");

  ResolvedDokanRemoveMountPoint = (DokanRemoveMountPointType)GetProcAddress(
      dokanDll, "DokanRemoveMountPoint");

  if(!ResolvedDokanMain
	  || !ResolvedDokanUnmount
	  || !ResolvedDokanRemoveMountPoint) {

	  ResolvedShutdown();
	  ResolvedShutdown = NULL;

	  return false;
  }

  return true;
}

fuse_chan::~fuse_chan() {
  
	if(dokanDll) {

		if(ResolvedShutdown) {

			ResolvedShutdown();
		}

		FreeLibrary(dokanDll);
	}
}

///////////////////////////////////////////////////////////////////////////////////////
////// This are just "emulators" of native FUSE api for the sake of compatibility
///////////////////////////////////////////////////////////////////////////////////////
#define FUSE_LIB_OPT(t, p, v)                                                  \
  { t, offsetof(struct fuse_config, p), v }

enum { KEY_HELP };

static const struct fuse_opt fuse_lib_opts[] = {
    FUSE_OPT_KEY("-h", KEY_HELP),
    FUSE_OPT_KEY("--help", KEY_HELP),
    FUSE_OPT_KEY("debug", FUSE_OPT_KEY_KEEP),
    FUSE_OPT_KEY("-d", FUSE_OPT_KEY_KEEP),
    FUSE_LIB_OPT("debug", debug, 1),
    FUSE_LIB_OPT("-d", debug, 1),
    FUSE_LIB_OPT("rdonly", readonly, 1),
    FUSE_LIB_OPT("-r", readonly, 1),
    FUSE_LIB_OPT("umask=%o", umask, 0),
    FUSE_LIB_OPT("fileumask=%o", fileumask, 0),
    FUSE_LIB_OPT("dirumask=%o", dirumask, 0),
    FUSE_LIB_OPT("fsname=%s", fsname, 0),
    FUSE_LIB_OPT("volname=%s", volname, 0),
    FUSE_LIB_OPT("uncname=%s", uncname, 0),
    FUSE_LIB_OPT("setsignals=%s", setsignals, 0),
    FUSE_LIB_OPT("daemon_timeout=%d", timeoutInSec, 0),
    FUSE_LIB_OPT("alloc_unit_size=%lu", allocationUnitSize, 0),
    FUSE_LIB_OPT("sector_size=%lu", sectorSize, 0),
    FUSE_LIB_OPT("-n", networkDrive, 1),
    FUSE_OPT_END};

static void fuse_lib_help(void) {
  fprintf(
      stderr,
      "    -o umask=M             set file and directory permissions (octal)\n"
      "    -o fileumask=M         set file permissions (octal)\n"
      "    -o dirumask=M          set directory permissions (octal)\n"
      "    -o fsname=M            set filesystem name\n"
      "    -o volname=M           set volume name\n"
      "    -o uncname=M           set UNC name\n"
      "    -o setsignals=M        set signal usage (1 to use)\n"
      "    -o daemon_timeout=M    set timeout in seconds\n"
      "    -o alloc_unit_size=M   set allocation unit size\n"
      "    -o sector_size=M       set sector size\n"
      "    -n                     use network drive\n"
      "\n");
}

static int fuse_lib_opt_proc(void *data, const char *arg, int key,
                             struct fuse_args *outargs) {
  (void)arg;
  (void)outargs;

  if (key == KEY_HELP) {
    struct fuse_config *conf = static_cast<struct fuse_config *>(data);
    fuse_lib_help();
    conf->help = 1;
  }

  return 1;
}

int fuse_is_lib_option(const char *opt) {
  return fuse_opt_match(fuse_lib_opts, opt);
}

int fuse_loop_mt(struct fuse *f) { return do_fuse_loop(f, true); }

int fuse_loop(struct fuse *f) { return do_fuse_loop(f, false); }

struct fuse_chan *fuse_mount(const char *mountpoint, struct fuse_args *args) {
  if (mountpoint == nullptr || mountpoint[0] == '\0')
    return nullptr;

  std::unique_ptr<fuse_chan> chan(new fuse_chan());
  // NOTE: we used to do chan->init() here to check that Dokan DLLs can be
  // loaded.
  // However, this does not live well with Cygwin. It's common for filesystem
  // drivers
  // to daemon()ize themselves (which involves fork() call) and forking doesn't
  // work
  // with Dokan. So defer loading until the main loop.

  chan->mountpoint = mountpoint;
  return chan.release();
}

void fuse_unmount(const char *mountpoint, struct fuse_chan *ch) {
  if (mountpoint == nullptr || mountpoint[0] == '\0')
    return;

  fuse_chan chan;
  if (!ch) {
    ch = &chan;
    ch->init();
    ch->mountpoint = mountpoint;
  }

  // Unmount attached FUSE filesystem
  if (ch->ResolvedDokanRemoveMountPoint) {
    wchar_t wmountpoint[MAX_PATH + 1];
    mbstowcs(wmountpoint, mountpoint, MAX_PATH);
    wchar_t &last = wmountpoint[wcslen(wmountpoint) - 1];
    if (last == L'\\' || last == L'/')
      last = L'\0';
    ch->ResolvedDokanRemoveMountPoint(wmountpoint);
    return;
  }
  if (ch->ResolvedDokanUnmount)
    ch->ResolvedDokanUnmount(mountpoint[0]); // Ugly :(
}

// Used from fuse_helpers.c
extern "C" int fuse_session_exit(struct fuse_session *se) {
  fuse_unmount(se->ch->mountpoint.c_str(), se->ch);
  return 0;
}

struct fuse *fuse_new(struct fuse_chan *ch, struct fuse_args *args,
                      const struct fuse_operations *op, size_t op_size,
                      void *user_data) {
  std::unique_ptr<fuse> res(new fuse());
  res->sess.ch = ch;
  res->ch.reset(ch); // Attach channel
  res->user_data = user_data;

  // prepare 'safe' options
  fuse_operations safe_ops = {nullptr};
  memcpy(&safe_ops, op,
         op_size > sizeof(safe_ops) ? sizeof(safe_ops) : op_size);
  res->ops = safe_ops;

  // Get debug param and filesystem name
  if (fuse_opt_parse(args, &res->conf, fuse_lib_opts, fuse_lib_opt_proc) == -1)
    return nullptr;
  // res->conf.debug=1;

  return res.release();
}

void fuse_exit(struct fuse *f) {
  // A hack - unmount the attached filesystem, it will cause the loop to end
  if (f == nullptr || !f->ch.get() || f->ch->mountpoint.empty())
    return;
  // Unmount attached FUSE filesystem
  f->ch->ResolvedDokanUnmount(f->ch->mountpoint.at(0)); // Ugly :(
}

void fuse_destroy(struct fuse *f) { delete f; }

struct fuse *fuse_setup(int argc, char *argv[],
                        const struct fuse_operations *op, size_t op_size,
                        char **mountpoint, int *multithreaded,
                        void *user_data) {
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct fuse_chan *ch = nullptr;
  struct fuse *fuse;
  int foreground;
  int res;

  res = fuse_parse_cmdline(&args, mountpoint, multithreaded, &foreground);
  if (res == -1)
    return nullptr;

  ch = fuse_mount(*mountpoint, &args);

  fuse = fuse_new(ch, &args, op, op_size, user_data);
  fuse_opt_free_args(&args);
  if (fuse == nullptr || ch == nullptr)
    goto err_unmount;

  res = fuse_daemonize(foreground);
  if (res == -1)
    goto err_unmount;

  if (fuse->conf.setsignals) {
    res = fuse_set_signal_handlers(fuse_get_session(fuse));
    if (res == -1)
      goto err_unmount;
  }

  return fuse;

err_unmount:
  fuse_unmount(*mountpoint, ch);
  if (fuse)
    fuse_destroy(fuse);
  free(*mountpoint);
  return nullptr;
}

void fuse_teardown(struct fuse *fuse, char *mountpoint) {
  struct fuse_session *se = fuse_get_session(fuse);
  struct fuse_chan *ch = se->ch;
  if (fuse->conf.setsignals)
    fuse_remove_signal_handlers(se);
  fuse_unmount(mountpoint, ch);
  fuse_destroy(fuse);
  free(mountpoint);
}

int fuse_exited(struct fuse *f) { return !f->within_loop; }

struct fuse_session *fuse_get_session(struct fuse *f) {
  return &f->sess;
}

int fuse_main_real(int argc, char *argv[], const struct fuse_operations *op,
                   size_t op_size, void *user_data) {
  struct fuse *fuse;
  char *mountpoint;
  int multithreaded;
  int res;

  fuse = fuse_setup(argc, argv, op, op_size, &mountpoint, &multithreaded,
                    user_data);
  if (fuse == nullptr)
    return 1;

  // MT loops are only supported on MSVC
  if (multithreaded)
    res = fuse_loop_mt(fuse);
  else
    res = fuse_loop(fuse);

  fuse_teardown(fuse, mountpoint);
  if (res < 0)
    return 1;

  return 0;
}
