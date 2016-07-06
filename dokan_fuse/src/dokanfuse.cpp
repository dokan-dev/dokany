#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <sddl.h>
#include "utils.h"
#include "fusemain.h"
#include "ScopeGuard.h"
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
      DokanFileInfo->DokanOptions->GlobalContext)

HINSTANCE hFuseDllInstance;

extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason,
                               LPVOID lpReserved) {
  if (dwReason == DLL_PROCESS_ATTACH) {
    hFuseDllInstance = hInstance;
    DisableThreadLibraryCalls(hInstance);
  }
  return TRUE;
}

static NTSTATUS DOKAN_CALLBACK
FuseFindFiles(LPCWSTR FileName,
              PFillFindData FillFindData, // function pointer
              PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "FindFiles: %ls\n", FileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(
      impl->find_files(FileName, FillFindData, DokanFileInfo));
}

static void DOKAN_CALLBACK FuseCleanup(LPCWSTR FileName,
                                       PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
	  FPRINTF(stderr, "Cleanup: %ls\n\n", FileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  impl->cleanup(FileName, DokanFileInfo);
}

static NTSTATUS DOKAN_CALLBACK
FuseDeleteDirectory(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
	  FPRINTF(stderr, "DeleteDirectory: %ls\n", FileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(
      impl->delete_directory(FileName, DokanFileInfo));
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
  while (c->name != NULL && c->value != value)
    ++c;
  fprintf(stderr, "%s: %s (%lx)\n", name, c->name ? c->name : "unknown!",
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
    fprintf(stderr, "%s0x%lX", sep, (long unsigned)left);
  fprintf(stderr, "\n");
}

static NTSTATUS DOKAN_CALLBACK
FuseCreateFile(LPCWSTR FileName, PDOKAN_IO_SECURITY_CONTEXT SecurityContext,
               ACCESS_MASK DesiredAccess, ULONG FileAttributes,
               ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions,
               PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;

  if (impl->debug()) {
    FPRINTF(stderr, "CreateFile: %ls\n", FileName);
    DebugConstantBit("\tDesiredAccess", DesiredAccess, cAccessMode);
    DebugConstantBit("\tShareAccess", ShareAccess, cShareMode);
    DebugConstant("\tDisposition", CreateDisposition, cDisposition);
    FPRINTF(stderr, "\tAttributes: %u (0x%x)\n", FileAttributes,
             FileAttributes);
    FPRINTF(stderr, "\tOptions: %u (0x%x)\n", CreateOptions, CreateOptions);
    fflush(stderr);
  }

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);

  if ((CreateOptions & FILE_DIRECTORY_FILE) == FILE_DIRECTORY_FILE) {

    if (CreateDisposition == FILE_CREATE || CreateDisposition == FILE_OPEN_IF) {
      return errno_to_ntstatus_error(
          impl->create_directory(FileName, DokanFileInfo));
    } else if (CreateDisposition == FILE_OPEN) {

      return errno_to_ntstatus_error(
          impl->open_directory(FileName, DokanFileInfo));
    }
  }

  return impl->create_file(FileName, DesiredAccess, ShareAccess,
                           CreateDisposition, FileAttributes,
                           DokanFileInfo);
}

static void DOKAN_CALLBACK FuseCloseFile(LPCWSTR FileName,
                                         PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "Close: %ls\n\n", FileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  impl->close_file(FileName, DokanFileInfo);
}

static NTSTATUS DOKAN_CALLBACK FuseReadFile(LPCWSTR FileName, LPVOID Buffer,
                                            DWORD BufferLength,
                                            LPDWORD ReadLength, LONGLONG Offset,
                                            PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "ReadFile: %ls from %lld len %u\n", FileName,
             (__int64)Offset, (unsigned)BufferLength);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(impl->read_file(
      FileName, Buffer, BufferLength, ReadLength, Offset, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseWriteFile(LPCWSTR FileName, LPCVOID Buffer,
                                             DWORD NumberOfBytesToWrite,
                                             LPDWORD NumberOfBytesWritten,
                                             LONGLONG Offset,
                                             PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "WriteFile: %ls, offset %lld, length %lu\n", FileName,
             Offset, NumberOfBytesToWrite);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(
      impl->write_file(FileName, Buffer, NumberOfBytesToWrite,
                       NumberOfBytesWritten, Offset, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK
FuseFlushFileBuffers(LPCWSTR FileName, PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "FlushFileBuffers: %ls\n", FileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(
      impl->flush_file_buffers(FileName, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseGetFileInformation(
    LPCWSTR FileName, LPBY_HANDLE_FILE_INFORMATION HandleFileInformation,
    PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "GetFileInfo: : %ls\n", FileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(impl->get_file_information(
      FileName, HandleFileInformation, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseDeleteFile(LPCWSTR FileName,
                                              PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "DeleteFile: %ls\n", FileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(impl->delete_file(FileName, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK
FuseMoveFile(LPCWSTR FileName, // existing file name
             LPCWSTR NewFileName, BOOL ReplaceIfExisting,
             PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "MoveFile: %ls -> %ls\n\n", FileName, NewFileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(
      impl->move_file(FileName, NewFileName, ReplaceIfExisting, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseLockFile(LPCWSTR FileName,
                                            LONGLONG ByteOffset,
                                            LONGLONG Length,
                                            PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "LockFile: %ls\n", FileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(
      impl->lock_file(FileName, ByteOffset, Length, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseUnlockFile(LPCWSTR FileName,
                                              LONGLONG ByteOffset,
                                              LONGLONG Length,
                                              PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "UnlockFile: %ls\n", FileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(
      impl->unlock_file(FileName, ByteOffset, Length, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseSetEndOfFile(
    LPCWSTR FileName, LONGLONG ByteOffset, PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "SetEndOfFile: %ls, %lld\n", FileName, ByteOffset);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(
      impl->set_end_of_file(FileName, ByteOffset, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseSetAllocationSize(
  LPCWSTR FileName, LONGLONG ByteOffset, PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "SetAllocationSize: %ls, %lld\n", FileName, ByteOffset);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);

  BY_HANDLE_FILE_INFORMATION byHandleFileInfo;
  ZeroMemory(&byHandleFileInfo, sizeof(BY_HANDLE_FILE_INFORMATION));

  NTSTATUS ret = errno_to_ntstatus_error(
      impl->get_file_information(FileName, &byHandleFileInfo, DokanFileInfo));

  LARGE_INTEGER fileSize;
  fileSize.LowPart = byHandleFileInfo.nFileSizeLow;
  fileSize.HighPart = byHandleFileInfo.nFileSizeHigh;

  if (ret != 0) {
    return ret;
  }
  else if (ByteOffset < fileSize.QuadPart) {
    /* https://msdn.microsoft.com/en-us/library/windows/hardware/ff540232(v=vs.85).aspx
    * The end-of-file position must always be less than or equal to the
    * allocation size. If the allocation size is set to a value that is
    * less than the end - of - file position, the end - of - file position
    * is automatically adjusted to match the allocation size.*/
    return errno_to_ntstatus_error(
        impl->set_end_of_file(FileName, ByteOffset, DokanFileInfo));
  }
  else {
    return 0;
  }
}

static NTSTATUS DOKAN_CALLBACK FuseSetFileAttributes(
    LPCWSTR FileName, DWORD FileAttributes, PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "SetFileAttributes: %ls\n", FileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(
      impl->set_file_attributes(FileName, FileAttributes, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseSetFileTime(LPCWSTR FileName,
                                               CONST FILETIME *CreationTime,
                                               CONST FILETIME *LastAccessTime,
                                               CONST FILETIME *LastWriteTime,
                                               PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "SetFileTime: %ls\n", FileName);

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(impl->set_file_time(
      FileName, CreationTime, LastAccessTime, LastWriteTime, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseGetDiskFreeSpace(
    PULONGLONG FreeBytesAvailable, PULONGLONG TotalNumberOfBytes,
    PULONGLONG TotalNumberOfFreeBytes, PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "GetDiskFreeSpace\n");

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(
      impl->get_disk_free_space(FreeBytesAvailable, TotalNumberOfBytes,
                                TotalNumberOfFreeBytes, DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK
GetVolumeInformation(LPWSTR VolumeNameBuffer, DWORD VolumeNameSize,
                     LPDWORD VolumeSerialNumber, LPDWORD MaximumComponentLength,
                     LPDWORD FileSystemFlags, LPWSTR FileSystemNameBuffer,
                     DWORD FileSystemNameSize, PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "GetVolumeInformation\n");

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  *VolumeSerialNumber = 0;
  *MaximumComponentLength = 255;
  return errno_to_ntstatus_error(impl->get_volume_information(
      VolumeNameBuffer, VolumeNameSize, FileSystemNameBuffer,
      FileSystemNameSize, DokanFileInfo, FileSystemFlags));
}

static NTSTATUS DOKAN_CALLBACK FuseMounted(PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "Mounted\n");

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(impl->mounted(DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK FuseUnmounted(PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "Unmount\n");

  impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
  return errno_to_ntstatus_error(impl->unmounted(DokanFileInfo));
}

static NTSTATUS DOKAN_CALLBACK
FuseGetFileSecurity(LPCWSTR FileName, PSECURITY_INFORMATION SecurityInformation,
                    PSECURITY_DESCRIPTOR SecurityDescriptor, ULONG BufferLength,
                    PULONG LengthNeeded, PDOKAN_FILE_INFO DokanFileInfo) {
  impl_fuse_context *impl = the_impl;
  if (impl->debug())
    FPRINTF(stderr, "GetFileSecurity: %x\n", *SecurityInformation);

  BY_HANDLE_FILE_INFORMATION byHandleFileInfo;
  ZeroMemory(&byHandleFileInfo, sizeof(BY_HANDLE_FILE_INFORMATION));

  int ret;
  {
    impl_chain_guard guard(impl, DokanFileInfo->ProcessId);
    ret =
        impl->get_file_information(FileName, &byHandleFileInfo, DokanFileInfo);
  }

  if (0 != ret) {
    return errno_to_ntstatus_error(ret);
  }

  if (byHandleFileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    // We handle directories for the Explorer's
    // context menu. (New Folder, ...)

    // SDDL used by dokan driver
    if (!ConvertStringSecurityDescriptorToSecurityDescriptor(
            "D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)(A;;GRGWGX;;;WD)(A;;GRGX;;;RC)",
            SDDL_REVISION_1, &SecurityDescriptor, &BufferLength)) {
      return STATUS_NOT_IMPLEMENTED;
    }

    LPTSTR pStringBuffer = NULL;
    if (!ConvertSecurityDescriptorToStringSecurityDescriptor(
            SecurityDescriptor, SDDL_REVISION_1, *SecurityInformation,
            &pStringBuffer, NULL)) {
      return STATUS_NOT_IMPLEMENTED;
    }

    if (!ConvertStringSecurityDescriptorToSecurityDescriptor(
            pStringBuffer, SDDL_REVISION_1, &SecurityDescriptor,
            &BufferLength)) {
      return STATUS_NOT_IMPLEMENTED;
    }

    if (pStringBuffer != NULL)
      LocalFree(pStringBuffer);

    return STATUS_SUCCESS;
  } else {
    return STATUS_NOT_IMPLEMENTED;
  }
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
    NULL, // FindFilesWithPattern
    FuseSetFileAttributes,
    FuseSetFileTime,
    FuseDeleteFile,
    FuseDeleteDirectory,
    FuseMoveFile,
    FuseSetEndOfFile,
    FuseSetAllocationSize,
    FuseLockFile,
    FuseUnlockFile,
    FuseGetDiskFreeSpace,
    GetVolumeInformation,
    FuseMounted,
    FuseUnmounted,
    FuseGetFileSecurity,
    NULL, // SetFileSecurity
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
                         fs->conf.volname);

  // Parse Dokan options
  PDOKAN_OPTIONS dokanOptions = (PDOKAN_OPTIONS)malloc(sizeof(DOKAN_OPTIONS));
  if (dokanOptions == NULL) {
    return -1;
  }
  ZeroMemory(dokanOptions, sizeof(DOKAN_OPTIONS));
  dokanOptions->Options |=
      fs->conf.networkDrive ? DOKAN_OPTION_NETWORK : DOKAN_OPTION_REMOVABLE;
  dokanOptions->GlobalContext = reinterpret_cast<ULONG64>(&impl);

  wchar_t mount[MAX_PATH + 1];
  mbstowcs(mount, fs->ch->mountpoint.c_str(), MAX_PATH);

  dokanOptions->Version = DOKAN_VERSION;
  dokanOptions->MountPoint = mount;
  dokanOptions->ThreadCount = mt ? FUSE_THREAD_COUNT : 1;
  dokanOptions->Timeout = fs->conf.timeoutInSec * 1000;

  // Debug
  if (fs->conf.debug)
    dokanOptions->Options |= DOKAN_OPTION_DEBUG | DOKAN_OPTION_STDERR;

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
  DokanVersionType ResolvedDokanVersion;
  ResolvedDokanVersion =
      (DokanVersionType)GetProcAddress(dokanDll, "DokanVersion");
  if (!ResolvedDokanVersion || ResolvedDokanVersion() < DOKAN_VERSION)
    return false;

  ResolvedDokanMain = (DokanMainType)GetProcAddress(dokanDll, "DokanMain");
  ResolvedDokanUnmount =
      (DokanUnmountType)GetProcAddress(dokanDll, "DokanUnmount");
  ResolvedDokanRemoveMountPoint = (DokanRemoveMountPointType)GetProcAddress(
      dokanDll, "DokanRemoveMountPoint");

  if (!ResolvedDokanMain || !ResolvedDokanUnmount ||
      !ResolvedDokanRemoveMountPoint)
    return false;
  return true;
}

fuse_chan::~fuse_chan() {
  if (dokanDll)
    FreeLibrary(dokanDll);
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
    FUSE_LIB_OPT("umask=%o", umask, 0),
    FUSE_LIB_OPT("fileumask=%o", fileumask, 0),
    FUSE_LIB_OPT("dirumask=%o", dirumask, 0),
    FUSE_LIB_OPT("fsname=%s", fsname, 0),
    FUSE_LIB_OPT("volname=%s", volname, 0),
    FUSE_LIB_OPT("setsignals=%s", setsignals, 0),
    FUSE_LIB_OPT("daemon_timeout=%d", timeoutInSec, 0),
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
      "    -o setsignals=M        set signal usage (1 to use)\n"
      "    -o daemon_timeout=M    set timeout in seconds\n"
      "    -n                     use network drive\n"
      "\n");
}

static int fuse_lib_opt_proc(void *data, const char *arg, int key,
                             struct fuse_args *outargs) {
  (void)arg;
  (void)outargs;

  if (key == KEY_HELP) {
    struct fuse_config *conf = (struct fuse_config *)data;
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
  if (mountpoint == NULL || mountpoint[0] == '\0')
    return NULL;

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
  if (mountpoint == NULL || mountpoint[0] == '\0')
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
  fuse_operations safe_ops = {0};
  memcpy(&safe_ops, op,
         op_size > sizeof(safe_ops) ? sizeof(safe_ops) : op_size);
  res->ops = safe_ops;

  // Get debug param and filesystem name
  if (fuse_opt_parse(args, &res->conf, fuse_lib_opts, fuse_lib_opt_proc) == -1)
    return NULL;
  // res->conf.debug=1;

  return res.release();
}

void fuse_exit(struct fuse *f) {
  // A hack - unmount the attached filesystem, it will cause the loop to end
  if (f == NULL || !f->ch.get() || f->ch->mountpoint.empty())
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
  struct fuse_chan *ch = NULL;
  struct fuse *fuse;
  int foreground;
  int res;

  res = fuse_parse_cmdline(&args, mountpoint, multithreaded, &foreground);
  if (res == -1)
    return NULL;

  ch = fuse_mount(*mountpoint, &args);

  fuse = fuse_new(ch, &args, op, op_size, user_data);
  fuse_opt_free_args(&args);
  if (fuse == NULL || ch == NULL)
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
  return NULL;
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
  if (fuse == NULL)
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
