#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <errno.h>
#include <sys/utime.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif
#include <map>

#include "fusemain.h"
#include "utils.h"

#ifndef S_ISLNK
#define S_ISLNK(mode) __S_ISTYPE((mode), __S_IFLNK)
#endif
#define __S_IFLNK 0120000 /* Symbolic link.  */
#define __S_ISTYPE(mode, mask) (((mode)&__S_IFMT) == (mask))
#define __S_IFMT 0170000 /* These bits determine file type.  */

#define ACCESS_READ                                                            \
  (STANDARD_RIGHTS_EXECUTE | STANDARD_RIGHTS_READ | GENERIC_READ |             \
   GENERIC_EXECUTE | FILE_GENERIC_EXECUTE | FILE_GENERIC_READ | READ_CONTROL | \
   FILE_EXECUTE | FILE_LIST_DIRECTORY | FILE_READ_DATA | FILE_READ_EA)
#define ACCESS_WRITE                                                           \
  (GENERIC_WRITE | WRITE_DAC | WRITE_OWNER | FILE_APPEND_DATA |                \
   FILE_WRITE_ATTRIBUTES | FILE_WRITE_DATA | FILE_WRITE_EA | FILE_ADD_FILE |   \
   FILE_ADD_SUBDIRECTORY | FILE_APPEND_DATA)

///////////////////////////////////////////////////////////////////////////////////////
////// FUSE frames chain
///////////////////////////////////////////////////////////////////////////////////////

#ifdef _MSC_VER
__declspec(thread) impl_chain_link *cur_impl_chain_link = nullptr;
#else
static __thread impl_chain_link *cur_impl_chain_link = NULL;
#endif

impl_chain_guard::impl_chain_guard(impl_fuse_context *ctx, int caller_pid) {
  link.call_ctx_.pid = caller_pid;
  link.call_ctx_.private_data = ctx->user_data_;
  link.call_ctx_.fuse = static_cast<struct fuse *>(static_cast<void *>(ctx)); // Hack, really...

  link.prev_link_ = cur_impl_chain_link;

  // Push current context on the chain stack.
  // Note, this is thread-safe since we work with a thread-local variable
  cur_impl_chain_link = &link;
}

impl_chain_guard::~impl_chain_guard() {
  if (&link != cur_impl_chain_link)
    abort(); //"FUSE frames stack is damaged!"
  cur_impl_chain_link = link.prev_link_;
}

struct fuse_context *fuse_get_context(void) {
  if (cur_impl_chain_link == nullptr)
    return nullptr;
  return &cur_impl_chain_link->call_ctx_;
}

///////////////////////////////////////////////////////////////////////////////////////
////// FUSE bridge
///////////////////////////////////////////////////////////////////////////////////////
impl_fuse_context::impl_fuse_context(const struct fuse_operations *ops,
                                     void *user_data, bool debug,
                                     unsigned int filemask,
                                     unsigned int dirmask, const char *fsname,
                                     const char *volname, const char *uncname)
    : ops_(*ops), user_data_(user_data), debug_(debug), filemask_(filemask),
      dirmask_(dirmask), fsname_(fsname),
      volname_(volname), uncname_(uncname) // Use current user data
{
  // Reset connection info
  memset(&conn_info_, 0, sizeof(fuse_conn_info));
  conn_info_.max_write = UINT_MAX;
  conn_info_.max_readahead = UINT_MAX;
  conn_info_.proto_major = FUSE_MAJOR_VERSION;
  conn_info_.proto_minor = FUSE_MINOR_VERSION;

  if (ops_.init) {
    // Create a special FUSE frame
    impl_chain_guard guard(this, -1);

    // Run constructor and replace private data
    user_data_ = ops_.init(&conn_info_);
  }
}

int impl_fuse_context::cast_from_longlong(LONGLONG src, FUSE_OFF_T *res) {
#ifndef WIDE_OFF_T
  if (src > LONG_MAX || src < LONG_MIN)
    return -E2BIG;
#endif
  *res = static_cast<FUSE_OFF_T>(src);
  return 0;
}

int impl_fuse_context::do_open_dir(LPCWSTR FileName,
                                   PDOKAN_FILE_INFO DokanFileInfo) {
  if (ops_.opendir) {
    std::string fname = unixify(wchar_to_utf8_cstr(FileName));
    std::unique_ptr<impl_file_handle> file;
    // TODO access_mode
    CHECKED(file_locks.get_file(
        fname, true, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        file));

    fuse_file_info finfo = {0};
    CHECKED(ops_.opendir(fname.c_str(), &finfo));

    file->set_finfo(finfo);
    DokanFileInfo->Context = reinterpret_cast<ULONG64>(file.release());
    return 0;
  }

  DokanFileInfo->Context = 0;
  return 0;
}

int impl_fuse_context::do_open_file(LPCWSTR FileName, DWORD share_mode,
                                    DWORD Flags,
                                    PDOKAN_FILE_INFO DokanFileInfo) {
  if (!ops_.open)
    return -EINVAL;
  std::string fname = unixify(wchar_to_utf8_cstr(FileName));
  CHECKED(check_and_resolve(&fname));

  std::unique_ptr<impl_file_handle> file;
  CHECKED(file_locks.get_file(fname, false, Flags, share_mode, file));

  fuse_file_info finfo = {0};
  finfo.flags = convert_flags(Flags);

  CHECKED(ops_.open(fname.c_str(), &finfo));

  file->set_finfo(finfo);
  DokanFileInfo->Context = reinterpret_cast<ULONG64>(file.release());
  return 0;
}

int impl_fuse_context::do_delete_directory(LPCWSTR file_name,
                                           PDOKAN_FILE_INFO dokan_file_info) {
  std::string fname = unixify(wchar_to_utf8_cstr(file_name));

  if (!ops_.rmdir || !ops_.getattr)
    return -EINVAL;

  // Make sure directory is NOT opened
  // TODO: potential race here - Unix filesystems typically allow
  // to delete open files and directories.
  impl_file_handle *hndl =
      reinterpret_cast<impl_file_handle *>(dokan_file_info->Context);
  if (hndl)
    return -EBUSY;

  // A special case: symlinks are deleted by unlink, not rmdir
  struct FUSE_STAT stbuf = {0};
  CHECKED(ops_.getattr(fname.c_str(), &stbuf));
  if (S_ISLNK(stbuf.st_mode) && ops_.unlink)
    return ops_.unlink(fname.c_str());

  // Ok, try to rmdir it.
  return ops_.rmdir(fname.c_str());
}

int impl_fuse_context::do_delete_file(LPCWSTR file_name,
                                      PDOKAN_FILE_INFO dokan_file_info) {
  if (!ops_.unlink)
    return -EINVAL;

  // Note: we do not try to resolve symlink target
  std::string fname = unixify(wchar_to_utf8_cstr(file_name));
  return ops_.unlink(fname.c_str());
}

int impl_fuse_context::do_create_file(LPCWSTR FileName, DWORD Disposition,
                                      DWORD share_mode, DWORD Flags,
                                      PDOKAN_FILE_INFO DokanFileInfo)
// Kernel mappsings:
// Disposition = CreateDisposition
// Flags = DesiredAccess
// share_mode = ShareAccess
{
  std::string fname = unixify(wchar_to_utf8_cstr(FileName));

  // Create file?
  if (Disposition != FILE_CREATE && Disposition != FILE_SUPERSEDE &&
      Disposition != FILE_OPEN_IF && Disposition != FILE_OVERWRITE_IF) {
    return -ENOENT; // No, we're trying to open an existing file!
  }

  if (!ops_.create) {
    // Use mknod+open.
    if (!ops_.mknod || !ops_.open)
      return -EINVAL;

    CHECKED(ops_.mknod(fname.c_str(), filemask_, 0));

    return do_open_file(FileName, share_mode, Flags, DokanFileInfo);
  }

  std::unique_ptr<impl_file_handle> file;
  CHECKED(file_locks.get_file(fname, false, Flags, share_mode, file));

  fuse_file_info finfo = {0};
  finfo.flags =
      O_CREAT | O_EXCL |
      convert_flags(Flags); // TODO: these flags should be OK for new files?

  CHECKED(ops_.create(fname.c_str(), filemask_, &finfo));

  file->set_finfo(finfo);
  DokanFileInfo->Context = reinterpret_cast<ULONG64>(file.release());
  return 0;
}

int impl_fuse_context::convert_flags(DWORD Flags) {
  bool read = (Flags & ACCESS_READ) != 0;
  bool write = (Flags & ACCESS_WRITE) != 0;
  if (read && !write)
    return O_RDONLY;
  if (!read && write)
    return O_WRONLY;
  return O_RDWR;
}

int impl_fuse_context::resolve_symlink(const std::string &name,
                                       std::string *res) {
  if (!ops_.readlink)
    return -EINVAL;

  char buf[MAX_PATH * 2] = {0};
  CHECKED(ops_.readlink(name.c_str(), buf, MAX_PATH * 2));
  if (buf[0] == '/')
    *res = buf;
  else {
    // TODO: add full path normalization here
    *res = extract_dir_name(name) + buf;
  }

  return 0;
}

int impl_fuse_context::check_and_resolve(std::string *name) {
  if (!ops_.getattr)
    return -EINVAL;

  struct FUSE_STAT stat = {0};
  CHECKED(ops_.getattr(name->c_str(), &stat));
  if (S_ISLNK(stat.st_mode)) {
    CHECKED(resolve_symlink(*name, name));
  }

  return 0;
}

int impl_fuse_context::walk_directory(void *buf, const char *name,
                                      const struct FUSE_STAT *stbuf,
                                      FUSE_OFF_T off) {
  walk_data *wd = static_cast<walk_data *>(buf);
  WIN32_FIND_DATAW find_data = {0};

  fuse_context* context = fuse_get_context();
  if(context == nullptr && wd->delegateSetFuseContext != nullptr)
  {
    return wd->delegateSetFuseContext(wd->DokanFileInfo,buf, name,stbuf, off);
  }

  impl_fuse_context* ctx = wd->ctx;
  PFillFindData p_fill_find_data = wd->delegate;
  PDOKAN_FILE_INFO DokanFileInfo = wd->DokanFileInfo;
  std::string dirname = wd->dirname;

  utf8_to_wchar_buf(name, find_data.cFileName, MAX_PATH);
  // fix name if wrong encoding
  if (!find_data.cFileName[0]) {
    struct FUSE_STAT stbuf = {0};
    utf8_to_wchar_buf_old(name, find_data.cFileName, MAX_PATH);
    std::string new_name = wchar_to_utf8_cstr(find_data.cFileName);
    if (ctx->ops_.getattr && ctx->ops_.rename && new_name.length() &&
        ctx->ops_.getattr(new_name.c_str(), &stbuf) == -ENOENT)
      ctx->ops_.rename(name, new_name.c_str());
  }
  memset(find_data.cAlternateFileName, 0, sizeof(find_data.cAlternateFileName));

  struct FUSE_STAT stat = {0};

  // stat (*stbuf) has only st_ino and st_mode -> request other info with getattr
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {// Special entries
    stat.st_mode |= S_IFDIR; // TODO: fill directory params here!!!
  }
  else if (ctx->ops_.getattr) {
    CHECKED(ctx->ops_.getattr((dirname + name).c_str(), &stat));
  }

  if (S_ISLNK(stat.st_mode)
      && ctx->ops_.getattr) {
    std::string resolved;
    CHECKED(ctx->resolve_symlink(dirname + name, &resolved));
    CHECKED(ctx->ops_.getattr(resolved.c_str(), &stat));
  }

  convertStatlikeBuf(&stat, name, &find_data);

  uint32_t attrs = 0xFFFFFFFFu;
  if (ctx->ops_.win_get_attributes)
      attrs = ctx->ops_.win_get_attributes((dirname + name).c_str());
  if (attrs != 0xFFFFFFFFu)
    find_data.dwFileAttributes = attrs;

  return p_fill_find_data(&find_data, DokanFileInfo);
}

int impl_fuse_context::walk_directory_getdir(fuse_dirh_t hndl, const char *name,
                                             int type, ino_t ino) {
  walk_data *wd = reinterpret_cast<walk_data *>(hndl);
  wd->getdir_data.push_back(name); // Add this name to list
  return 0; // Get more entries
}

int impl_fuse_context::find_files(LPCWSTR file_name,
                                  PFillFindData fill_find_data,
                                  PWalkDirectoryWithSetFuseContext walk_set_fuse_context,
                                  PDOKAN_FILE_INFO dokan_file_info) {
  if ((!ops_.readdir && !ops_.getdir) || !ops_.getattr)
    return -EINVAL;

  std::string fname = unixify(wchar_to_utf8_cstr(file_name));
  CHECKED(check_and_resolve(&fname));

  walk_data wd;
  wd.ctx = this;
  wd.dirname = fname;
  if (*fname.rbegin() != '/')
    wd.dirname.append("/");
  wd.delegate = fill_find_data;
  wd.delegateSetFuseContext = walk_set_fuse_context;
  wd.DokanFileInfo = dokan_file_info;

  if (ops_.readdir) {
    impl_file_handle *hndl =
        reinterpret_cast<impl_file_handle *>(dokan_file_info->Context);
    if (hndl != nullptr) {
      fuse_file_info finfo(hndl->make_finfo());
      return ops_.readdir(fname.c_str(), &wd, &walk_directory, 0, &finfo);
    } else
      return ops_.readdir(fname.c_str(), &wd, &walk_directory, 0, nullptr);
  } else {
    CHECKED(
        ops_.getdir(fname.c_str(), reinterpret_cast<fuse_dirh_t>(&wd), &walk_directory_getdir));
    // Convert returned data The getdir_data array will be filled during
    // getdir() call.
    // We emulate FUSE behavior and do not pass information directly to Dokan
    // in walk_directory_getdir callback. This can cause excessive network
    // traffic
    // in sshfs because it populates stat buffer cache AFTER calling our
    // callback.
    // See: cache.c file, function cache_dirfill() in SSHFS 2.2
    for (std::vector<std::string>::const_iterator f = wd.getdir_data.begin();
         f != wd.getdir_data.end(); ++f)
      CHECKED(walk_directory(&wd, f->c_str(), nullptr, 0));
  }

  return 0;
}

int impl_fuse_context::open_directory(LPCWSTR file_name,
                                      PDOKAN_FILE_INFO dokan_file_info) {
  std::string fname = unixify(wchar_to_utf8_cstr(file_name));

  if (ops_.opendir)
    return do_open_dir(file_name, dokan_file_info);

  // We don't have opendir(), so the most we can do is make sure
  // that the target is indeed a directory
  struct FUSE_STAT st = {0};
  CHECKED(ops_.getattr(fname.c_str(), &st));
  if (S_ISLNK(st.st_mode)) {
    std::string resolved;
    CHECKED(resolve_symlink(fname, &resolved));
    CHECKED(ops_.getattr(resolved.c_str(), &st));
  }

  // Not a directory
  if ((st.st_mode & S_IFDIR) != S_IFDIR)
    return -ENOTDIR;

  dokan_file_info->Context = 0; // Do not want to attach anything
  return 0; // Use readdir here?
}

int impl_fuse_context::cleanup(LPCWSTR file_name,
                               PDOKAN_FILE_INFO dokan_file_info) {
  // TODO:
  // There's a subtle race condition possible here. 'Cleanup' is called when the
  // system closes the last handle from user space. However, there might still
  // be outstanding handles from kernel-space. So when userspace tries to
  // make CreateFile call - it might get error because the file is still locked
  // by the kernel space.

  // The one way to solve this is to keep a table of files 'still in flight'
  // and block until the file is closed. We're not doing this yet.

  // No context for directories when ops_.opendir is not set
  if (dokan_file_info->Context
    || (dokan_file_info->IsDirectory && !ops_.opendir)) {
    if (dokan_file_info->DeleteOnClose) {
      close_file(file_name, dokan_file_info);
      if (dokan_file_info->IsDirectory) {
        do_delete_directory(file_name, dokan_file_info);
      } else {
        do_delete_file(file_name, dokan_file_info);
      }
    }
  }

  return 0;
}

int impl_fuse_context::create_directory(LPCWSTR file_name,
                                        PDOKAN_FILE_INFO dokan_file_info) {
  std::string fname = unixify(wchar_to_utf8_cstr(file_name));

  if (!ops_.mkdir)
    return -EINVAL;

  return ops_.mkdir(fname.c_str(), dirmask_);
}

int impl_fuse_context::readdir_filler_set_has_files(void *buf, const char *name,
                                      const struct FUSE_STAT *stbuf,
                                      FUSE_OFF_T off) {
  bool *has_files = static_cast<bool *>(buf);
  int ret = 0;
  if (strcmp(name, ".") && strcmp(name, "..")) {
    *has_files = true;
    ret = 1;
  }
  return ret;
}

int impl_fuse_context::getdir_filler_set_has_files(fuse_dirh_t hndl, const char *name,
                                             int type, ino_t ino) {
    bool *has_files = reinterpret_cast<bool *>(hndl);
    int ret = 0;
    if (readdir_filler_set_has_files(has_files, name, nullptr, 0))
      ret = -ECANCELED;
    return ret;
}


int impl_fuse_context::delete_directory(LPCWSTR file_name,
                                        PDOKAN_FILE_INFO dokan_file_info) {
  std::string fname = unixify(wchar_to_utf8_cstr(file_name));
  if (!ops_.getattr || !ops_.rmdir || (!ops_.readdir && !ops_.getdir))
    return -EINVAL;

  struct FUSE_STAT stbuf = {0};
  int ret = ops_.getattr(fname.c_str(), &stbuf);

  /* TODO: Should we check if the parent-dir is writable and return -EACCESS?
   * (by using ops_.access or alternatively other means such as getattr)
   */
  if (ret < 0)
    return ret;

  bool has_files = false;
  if (ops_.readdir) {
    impl_file_handle *hndl = reinterpret_cast<impl_file_handle *>(dokan_file_info->Context);
    fuse_file_info *p_finfo = nullptr;
    fuse_file_info finfo;
    if (hndl != nullptr) {
      finfo = hndl->make_finfo();
      p_finfo = &finfo;
    }
    ret = ops_.readdir(fname.c_str(), &has_files, &readdir_filler_set_has_files, 0, p_finfo);
  } else {
    ret = ops_.getdir(fname.c_str(), reinterpret_cast<fuse_dirh_t>(&has_files), &getdir_filler_set_has_files);
    /* We only propagate the return value of getdir() when has_files is set.
     * To avoid reading the whole directory, we return an error in our callback
     * and set has_files after we encountered the first regular directory entry.
     * It is not exactly clear what getdir() is supposed to return in this case,
     * but given that we return -ENOTEMPTY anyways, we do not need to care.
     */
  }
  if (has_files) {
    ret = -ENOTEMPTY;
  }

  return ret;
}

win_error impl_fuse_context::create_file(LPCWSTR file_name, DWORD access_mode,
                                         DWORD share_mode,
                                         DWORD creation_disposition,
                                         DWORD flags_and_attributes,
                                         ULONG CreateOptions,
                                         PDOKAN_FILE_INFO dokan_file_info) {
  std::string fname = unixify(wchar_to_utf8_cstr(file_name));
  dokan_file_info->Context = 0;

  if (!ops_.getattr)
    return -EINVAL;

  struct FUSE_STAT stbuf = {0};
  // Check if the target file/directory exists
  if (ops_.getattr(fname.c_str(), &stbuf) < 0) {
    // Nope.
    if (dokan_file_info->IsDirectory)
      return -EINVAL; // We can't create directories using CreateFile
    return do_create_file(file_name, creation_disposition, share_mode,
                          access_mode, dokan_file_info);
  } else {
    if (S_ISLNK(stbuf.st_mode)) {
      // Get link's target
      CHECKED(resolve_symlink(fname, &fname));
      CHECKED(ops_.getattr(fname.c_str(), &stbuf));
    }

    if ((stbuf.st_mode & S_IFDIR) == S_IFDIR) {
      // Existing directory
      // TODO: add access control
      dokan_file_info->IsDirectory = TRUE;
      if (CreateOptions & FILE_NON_DIRECTORY_FILE)
        return -ENOENT;
      return do_open_dir(file_name, dokan_file_info);
    } else {
      // Existing file
      // Check if we'll be able to truncate or delete the opened file
      // TODO: race condition here?
      if (creation_disposition == FILE_OVERWRITE) {
        if (!ops_.unlink)
          return -EINVAL;
        CHECKED(ops_.unlink(fname.c_str())); // Delete file
        // And create it!
        return do_create_file(file_name, creation_disposition, share_mode,
                              access_mode, dokan_file_info);
      } else if (creation_disposition == FILE_SUPERSEDE ||
                 creation_disposition == FILE_OVERWRITE_IF) {
        if (!ops_.truncate)
          return -EINVAL;
        CHECKED(ops_.truncate(fname.c_str(), 0));
      } else if (creation_disposition == FILE_CREATE) {
        return win_error(STATUS_OBJECT_NAME_COLLISION, true);
      }

      long res = do_open_file(file_name, share_mode, access_mode, dokan_file_info);
      if (res == 0 &&
        (creation_disposition == FILE_OVERWRITE_IF ||
        creation_disposition == FILE_OPEN_IF)) {
        return win_error(STATUS_OBJECT_NAME_COLLISION, true);
      }

      return res;
    }
  }
}

int impl_fuse_context::close_file(LPCWSTR file_name,
                                  PDOKAN_FILE_INFO dokan_file_info) {
  impl_file_handle *hndl =
      reinterpret_cast<impl_file_handle *>(dokan_file_info->Context);

  int flush_err = 0;
  if (hndl) {
    flush_err = hndl->close(&ops_);
    delete hndl;
  }
  dokan_file_info->Context = 0;

  return flush_err;
}

int impl_fuse_context::read_file(LPCWSTR /*file_name*/, LPVOID buffer,
                                 DWORD num_bytes_to_read, LPDWORD read_bytes,
                                 LONGLONG offset,
                                 PDOKAN_FILE_INFO dokan_file_info) {
  // Please note, that we ignore file_name here, because it might
  // have been retargeted by a symlink.
  if (!ops_.read)
    return -EINVAL;

  *read_bytes = 0; // Conform to ReadFile semantics

  impl_file_handle *hndl =
      reinterpret_cast<impl_file_handle *>(dokan_file_info->Context);
  if (!hndl)
    return -EINVAL;
  if (hndl->is_dir())
    return -EACCES;

  // check locking
  if (hndl->check_lock(offset, num_bytes_to_read))
    return -EACCES;

  FUSE_OFF_T off;
  CHECKED(cast_from_longlong(offset, &off));
  fuse_file_info finfo(hndl->make_finfo());

  DWORD total_read = 0;
  while (total_read < num_bytes_to_read) {
    DWORD to_read = num_bytes_to_read - total_read;
    if (to_read > MAX_READ_SIZE)
      to_read = MAX_READ_SIZE;

    int res = ops_.read(hndl->get_name().c_str(), static_cast<char *>(buffer), to_read, off,
                        &finfo);
    if (res < 0)
      return res; // Error
    if (res == 0)
      break; // End of file reached

    total_read += res;
    off += res;
    buffer = static_cast<char *>(buffer) + res;
  }
  // OK!
  *read_bytes = total_read;
  return 0;
}

int impl_fuse_context::write_file(LPCWSTR /*file_name*/, LPCVOID buffer,
                                  DWORD num_bytes_to_write,
                                  LPDWORD num_bytes_written, LONGLONG offset,
                                  PDOKAN_FILE_INFO dokan_file_info) {
  // Please note, that we ignore file_name here, because it might
  // have been retargeted by a symlink.

  *num_bytes_written = 0; // Conform to ReadFile semantics

  if (!ops_.write)
    return -EINVAL;

  impl_file_handle *hndl =
      reinterpret_cast<impl_file_handle *>(dokan_file_info->Context);
  if (!hndl)
    return -EINVAL;
  if (hndl->is_dir())
    return -EACCES;

  if (offset < 0) {
	  struct FUSE_STAT stat;
	  if (0 == ops_.getattr(hndl->get_name().c_str(), &stat)) {
		  offset = stat.st_size;
	  }
  }
  // Clip the maximum write size
  if (num_bytes_to_write > conn_info_.max_write)
    num_bytes_to_write = conn_info_.max_write;

  // check locking
  if (hndl->check_lock(offset, num_bytes_to_write))
    return -EACCES;

  FUSE_OFF_T off;
  CHECKED(cast_from_longlong(offset, &off));

  fuse_file_info finfo(hndl->make_finfo());
  int res = ops_.write(hndl->get_name().c_str(), static_cast<const char *>(buffer),
                       num_bytes_to_write, off, &finfo);
  if (res < 0)
    return res; // Error

  // OK!
  *num_bytes_written = res;
  return 0;
}

int impl_fuse_context::flush_file_buffers(LPCWSTR /*file_name*/,
                                          PDOKAN_FILE_INFO dokan_file_info) {
  // Please note, that we ignore file_name here, because it might
  // have been retargeted by a symlink.
  impl_file_handle *hndl =
      reinterpret_cast<impl_file_handle *>(dokan_file_info->Context);
  if (!hndl)
    return -EINVAL;

  if (hndl->is_dir()) {
    if (!ops_.fsyncdir)
      return -EINVAL;
    fuse_file_info finfo(hndl->make_finfo());
    return ops_.fsyncdir(hndl->get_name().c_str(), 0, &finfo);
  } else {
    if (!ops_.fsync)
      return -EINVAL;
    fuse_file_info finfo(hndl->make_finfo());
    return ops_.fsync(hndl->get_name().c_str(), 0, &finfo);
  }
}

int impl_fuse_context::get_file_information(
    LPCWSTR file_name, LPBY_HANDLE_FILE_INFORMATION handle_file_information,
    PDOKAN_FILE_INFO dokan_file_info) {
  std::string fname = unixify(wchar_to_utf8_cstr(file_name));

  if (!ops_.getattr)
    return -EINVAL;

  struct FUSE_STAT st = {0};
  CHECKED(ops_.getattr(fname.c_str(), &st));
  if (S_ISLNK(st.st_mode)) {
    std::string resolved;
    CHECKED(resolve_symlink(fname, &resolved));
    CHECKED(ops_.getattr(resolved.c_str(), &st));
  }

  handle_file_information->nNumberOfLinks = st.st_nlink;
  if ((st.st_mode & S_IFDIR) == S_IFDIR)
    dokan_file_info->IsDirectory = TRUE;
  convertStatlikeBuf(&st, fname, handle_file_information);

  uint32_t attrs = 0xFFFFFFFFu;
  if (ops_.win_get_attributes)
    attrs = ops_.win_get_attributes(fname.c_str());
  if (attrs != 0xFFFFFFFFu)
    handle_file_information->dwFileAttributes = attrs;

  return 0;
}

int impl_fuse_context::delete_file(LPCWSTR file_name,
                                   PDOKAN_FILE_INFO dokan_file_info) {
  std::string fname = unixify(wchar_to_utf8_cstr(file_name));

  if (!ops_.getattr)
    return -EINVAL;

  struct FUSE_STAT stbuf = {0};
  return ops_.getattr(fname.c_str(), &stbuf);
}

int impl_fuse_context::move_file(LPCWSTR file_name, LPCWSTR new_file_name,
                                 BOOL replace_existing,
                                 PDOKAN_FILE_INFO dokan_file_info) {
  if (!ops_.rename || !ops_.getattr)
    return -EINVAL;

  std::string name = unixify(wchar_to_utf8_cstr(file_name));
  std::string new_name = unixify(wchar_to_utf8_cstr(new_file_name));

  struct FUSE_STAT stbuf = {0};
  if (ops_.getattr(new_name.c_str(), &stbuf) != -ENOENT) {
    if (!replace_existing)
      return -EEXIST;

    // Cannot delete directory
    if ((stbuf.st_mode & S_IFDIR) != 0)
      return -EISDIR;
    if (!ops_.unlink)
      return -EINVAL;
    CHECKED(ops_.unlink(new_name.c_str()));
  }

  // this can happen cause DeleteFile in Windows can return success even if
  // file is still in the file system
  if (ops_.getattr(new_name.c_str(), &stbuf) != -ENOENT) {
    return -EEXIST;
  }

  CHECKED(ops_.rename(name.c_str(), new_name.c_str()));
  file_locks.renamed_file(name, new_name);
  return 0;
}

int impl_fuse_context::lock_file(LPCWSTR file_name, LONGLONG byte_offset,
                                 LONGLONG length,
                                 PDOKAN_FILE_INFO dokan_file_info) {
  impl_file_handle *hndl =
      reinterpret_cast<impl_file_handle *>(dokan_file_info->Context);
  if (!hndl)
    return -EINVAL;
  if (hndl->is_dir())
    return -EACCES;

  FUSE_OFF_T off;
  CHECKED(cast_from_longlong(byte_offset, &off));

  if (ops_.lock) {
    fuse_file_info finfo(hndl->make_finfo());

    struct flock lock;
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = off;
    lock.l_len = length;

    return ops_.lock(hndl->get_name().c_str(), &finfo, F_SETLK, &lock);
  }

  return hndl->lock(byte_offset, length);
}

int impl_fuse_context::unlock_file(LPCWSTR file_name, LONGLONG byte_offset,
                                   LONGLONG length,
                                   PDOKAN_FILE_INFO dokan_file_info) {
  impl_file_handle *hndl =
      reinterpret_cast<impl_file_handle *>(dokan_file_info->Context);
  if (!hndl)
    return -EINVAL;
  if (hndl->is_dir())
    return -EACCES;

  FUSE_OFF_T off;
  CHECKED(cast_from_longlong(byte_offset, &off));

  if (ops_.lock) {
    fuse_file_info finfo(hndl->make_finfo());

    struct flock lock;
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = off;
    lock.l_len = length;

    return ops_.lock(hndl->get_name().c_str(), &finfo, F_SETLK, &lock);
  }

  return hndl->unlock(byte_offset, length);
}

int impl_fuse_context::set_end_of_file(LPCWSTR file_name, LONGLONG byte_offset,
                                       PDOKAN_FILE_INFO dokan_file_info) {
  FUSE_OFF_T off;
  CHECKED(cast_from_longlong(byte_offset, &off));
  std::string fname = unixify(wchar_to_utf8_cstr(file_name));
  CHECKED(check_and_resolve(&fname));

  impl_file_handle *hndl =
      reinterpret_cast<impl_file_handle *>(dokan_file_info->Context);
  if (hndl && ops_.ftruncate) {
    fuse_file_info finfo(hndl->make_finfo());
    return ops_.ftruncate(hndl->get_name().c_str(), off, &finfo);
  }

  if (!ops_.truncate)
    return -EINVAL;
  return ops_.truncate(fname.c_str(), off);
}

int impl_fuse_context::set_file_attributes(LPCWSTR file_name,
                                           DWORD file_attributes,
                                           PDOKAN_FILE_INFO dokan_file_info) {
  // This method is unlikely to be implemented since we do not support
  // advanced properties
  // TODO: maybe use extended properties of underlying FS?

  // Just return 'success' since returning -EINVAL interferes with modification
  // time
  // setting from FAR Manager.
  if (ops_.win_set_attributes) {
    std::string fname = unixify(wchar_to_utf8_cstr(file_name));
    CHECKED(check_and_resolve(&fname));
    return ops_.win_set_attributes(fname.c_str(), file_attributes);
  }
  return 0;
}

int impl_fuse_context::helper_set_time_struct(const FILETIME *filetime,
                                              const time_t backup,
                                              time_t *dest) {
  if (is_filetime_set(filetime))
    *dest = filetimeToUnixTime(filetime);
  else if (backup != 0)
    *dest = backup;
  else
    return -EINVAL;

  return 0;
}

int impl_fuse_context::set_file_time(PCWSTR file_name,
                                     const FILETIME *creation_time,
                                     const FILETIME *last_access_time,
                                     const FILETIME *last_write_time,
                                     PDOKAN_FILE_INFO dokan_file_info) {
  if (!ops_.utimens && !ops_.utime && !ops_.win_set_times)
    return -EINVAL;

  if (ops_.win_set_times) {
    std::string fname = unixify(wchar_to_utf8_cstr(file_name));
    CHECKED(check_and_resolve(&fname));

    impl_file_handle *hndl =
        reinterpret_cast<impl_file_handle *>(dokan_file_info->Context);
    if (!hndl)
      return ops_.win_set_times(fname.c_str(), nullptr, creation_time,
                                last_access_time, last_write_time);

    if (hndl->is_dir())
      return -EACCES;

    fuse_file_info finfo(hndl->make_finfo());

    return ops_.win_set_times(fname.c_str(), &finfo, creation_time,
                              last_access_time, last_write_time);
  }

  if (!ops_.getattr)
    return -EINVAL;

  std::string fname = unixify(wchar_to_utf8_cstr(file_name));
  CHECKED(check_and_resolve(&fname));

  struct FUSE_STAT st = {0};
  CHECKED(ops_.getattr(fname.c_str(), &st));

  if (ops_.utimens) {
    struct timespec tv[2] = {0};
    // TODO: support nanosecond resolution
    // Access time
    CHECKED(helper_set_time_struct(last_access_time, st.st_atim.tv_sec,
                                   &(tv[0].tv_sec)));
    // Modification time
    CHECKED(helper_set_time_struct(last_write_time, st.st_mtim.tv_sec,
                                   &(tv[1].tv_sec)));

    return ops_.utimens(fname.c_str(), tv);
  } else {
    struct utimbuf ut = {0};
    // Access time
    CHECKED(helper_set_time_struct(last_access_time, st.st_atim.tv_sec,
                                   &(ut.actime)));
    // Modification time
    CHECKED(helper_set_time_struct(last_write_time, st.st_mtim.tv_sec,
                                   &(ut.modtime)));

    return ops_.utime(fname.c_str(), &ut);
  }
}

int impl_fuse_context::get_disk_free_space(PULONGLONG free_bytes_available,
                                           PULONGLONG number_of_bytes,
                                           PULONGLONG number_of_free_bytes,
                                           PDOKAN_FILE_INFO dokan_file_info) {
  if (!ops_.statfs) {
    *free_bytes_available = 0;
    *number_of_bytes = 0;
    *number_of_free_bytes = 0;
    return 0;
  }

  struct statvfs vfs = {0};
  CHECKED(ops_.statfs("/", &vfs));

  if (free_bytes_available != nullptr)
    *free_bytes_available = uint64_t(vfs.f_bsize) * vfs.f_bavail;
  if (number_of_free_bytes != nullptr)
    *number_of_free_bytes = uint64_t(vfs.f_bsize) * vfs.f_bfree;
  if (number_of_bytes != nullptr)
    *number_of_bytes = uint64_t(vfs.f_bsize) * vfs.f_blocks;

  return 0;
}

int impl_fuse_context::get_volume_information(LPWSTR volume_name_buffer,
                                              DWORD volume_name_size,
                                              LPWSTR file_system_name_buffer,
                                              DWORD file_system_name_size,
                                              PDOKAN_FILE_INFO dokan_file_info,
                                              LPDWORD volume_flags) {
  // case sensitive
  *volume_flags = 3;

  if (volname_)
    utf8_to_wchar_buf(volname_, volume_name_buffer, volume_name_size);
  else
    utf8_to_wchar_buf(DEFAULT_FUSE_VOLUME_NAME, volume_name_buffer,
                      volume_name_size);

  if (fsname_)
    utf8_to_wchar_buf(fsname_, file_system_name_buffer, file_system_name_size);
  else
    utf8_to_wchar_buf(DEFAULT_FUSE_FILESYSTEM_NAME, file_system_name_buffer,
                      file_system_name_size);

  return 0;
}

int impl_fuse_context::mounted(PDOKAN_FILE_INFO DokanFileInfo) {
	return 0;
}

int impl_fuse_context::unmounted(PDOKAN_FILE_INFO DokanFileInfo) {
  if (ops_.destroy)
    ops_.destroy(user_data_); // Ignoring result
  return 0;
}

///////////////////////////////////////////////////////////////////////////////////////
////// File lock
///////////////////////////////////////////////////////////////////////////////////////

// get required shared mode given an access mode
static DWORD required_share(DWORD access_mode) {
  DWORD share = 0;
  if (access_mode & (FILE_EXECUTE | FILE_READ_DATA))
    share |= FILE_SHARE_READ;
  if (access_mode & (FILE_WRITE_DATA | FILE_APPEND_DATA))
    share |= FILE_SHARE_WRITE;
  if (access_mode & DELETE)
    share |= FILE_SHARE_DELETE;
  return share;
}

int impl_file_locks::get_file(const std::string &name, bool is_dir,
                              DWORD access_mode, DWORD shared_mode,
                              std::unique_ptr<impl_file_handle> &file) {
  int res = 0;
  file.reset(new impl_file_handle(is_dir, shared_mode));

  // check previous files with same names
  impl_file_lock *lock, *old_lock = nullptr;
  EnterCriticalSection(&this->lock);
  file_locks_t::iterator i = file_locks.find(name);
  if (i != file_locks.end()) {
    old_lock = lock = i->second;
    EnterCriticalSection(&lock->lock);
  } else {
    lock = new impl_file_lock(this, name);
    file_locks[name] = lock;
    lock->add_file_unlocked(file.get());
  }
  file->file_lock = lock;

  if (!old_lock) {
    LeaveCriticalSection(&this->lock);
    return res;
  }

  // check previous files with same names
  DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  for (impl_file_handle *p = lock->first; p; p = p->next_file)
	  if (file.get() != p) {
		  share &= p->shared_mode_;
	  }
  if ((required_share(access_mode) | share) != share) {
    file.reset();
    res = -EACCES;
  } else {
    lock->add_file_unlocked(file.get());
  }
  LeaveCriticalSection(&lock->lock);
  LeaveCriticalSection(&this->lock);
  return res;
}

void impl_file_lock::add_file_unlocked(impl_file_handle *file) {
  file->next_file = first;
  first = file;
}

void impl_file_lock::remove_file(impl_file_handle *file) {
  impl_file_handle *first_locked;

  EnterCriticalSection(&lock);
  impl_file_handle **p = &first;
  while (*p != nullptr) {
    if (*p == file) {
      *p = file->next_file;
      file->next_file = nullptr;
      continue;
    }
    p = &(*p)->next_file;
  }
  first_locked = first;
  // avoid dead lock
  LeaveCriticalSection(&lock);

  // empty ??
  if (first_locked)
    return;

  locks->remove_file(name_);
}

void impl_file_locks::remove_file(const std::string &name) {
  EnterCriticalSection(&lock);
  file_locks_t::iterator i = file_locks.find(name);
  if (i != file_locks.end() && !i->second->first) {
    if (i->second)
      delete i->second;
    file_locks.erase(i);
  }
  LeaveCriticalSection(&lock);
}

void impl_file_locks::renamed_file(const std::string &name,
                                   const std::string &new_name) {
  if (name == new_name)
    return;

  EnterCriticalSection(&lock);
  // TODO what happen if new_name exists ??
  file_locks_t::iterator i = file_locks.find(name);
  if (i != file_locks.end()) {
    impl_file_lock *lock = i->second;
    EnterCriticalSection(&lock->lock);
    lock->name_ = new_name;
    LeaveCriticalSection(&lock->lock);
    file_locks[new_name] = lock;
    file_locks.erase(i);
  }
  LeaveCriticalSection(&lock);
}

int impl_file_lock::lock_file(impl_file_handle *file, long long start,
                              long long len, bool mark) {
  if (start < 0 || len <= 0)
    return -EINVAL;

  bool locked = false;
  EnterCriticalSection(&lock);
  // multiple locks are not allowed
  for (impl_file_handle *i = first; i; i = i->next_file) {
    if (!mark && i == file)
      continue;
    impl_file_handle::locks_t::iterator j = i->locks.lower_bound(start);
    if (j != i->locks.end()) {
      // we found a range which start after our start
      if (len > j->first - start)
        locked = true;
      // check previous not override
      if (j != i->locks.begin()) {
        --j;
        if (j->second > start - j->first)
          locked = true;
      }
    } else {
      // check last
      impl_file_handle::locks_t::reverse_iterator p = i->locks.rbegin();
      if (p != i->locks.rend() && start - p->first < p->second)
        locked = true;
    }
  }
  if (!locked & mark)
    file->locks[start] = len;
  LeaveCriticalSection(&lock);
  return locked ? -EACCES : 0;
}

int impl_file_lock::unlock_file(impl_file_handle *file, long long start,
                                long long len) {
  if (len == 0)
    return 0;

  if (start < 0 || len <= 0)
    return -EINVAL;

  EnterCriticalSection(&lock);
  bool locked = false;
  impl_file_handle::locks_t::iterator i = file->locks.find(start);
  if (i != file->locks.end()) {
    // we found a range which start as our, is our ??
    if (i->second == len) {
      file->locks.erase(i);
      locked = true;
    }
  }
  LeaveCriticalSection(&lock);
  return locked ? 0 : -EACCES;
}

///////////////////////////////////////////////////////////////////////////////////////
////// File handle
///////////////////////////////////////////////////////////////////////////////////////
impl_file_handle::impl_file_handle(bool is_dir, DWORD shared_mode)
    : is_dir_(is_dir), fh_(-1), next_file(nullptr), file_lock(nullptr), shared_mode_(shared_mode) {}

impl_file_handle::~impl_file_handle() { file_lock->remove_file(this); }

int impl_file_handle::close(const struct fuse_operations *ops) {
  int flush_err = 0;
  if (is_dir_) {
    if (ops->releasedir) {
      fuse_file_info finfo(make_finfo());
      ops->releasedir(get_name().c_str(), &finfo);
    }
  } else {
    if (ops->flush) {
      fuse_file_info finfo(make_finfo());
      finfo.flush = 1;
      flush_err = ops->flush(get_name().c_str(), &finfo);
    }
    if (ops->release) // Ignoring result.
    {
      fuse_file_info finfo(make_finfo());
      ops->release(get_name().c_str(), &finfo); // Set open() flags here?
    }
  }
  return flush_err;
}

fuse_file_info impl_file_handle::make_finfo() {
  fuse_file_info res = {0};
  res.fh = fh_;
  return res;
}
