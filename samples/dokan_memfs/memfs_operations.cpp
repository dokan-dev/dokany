/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2019 Adrien J. <liryna.stark@gmail.com>
  Copyright (C) 2020 Google, Inc.

  http://dokan-dev.github.io

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "memfs_operations.h"
#include "memfs_helper.h"

#include <sddl.h>
#include <spdlog/spdlog.h>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace memfs {
static const DWORD g_volumserial = 0x19831116;

static NTSTATUS create_main_stream(
    fs_filenodes* fs_filenodes, const std::wstring& filename,
    const std::pair<std::wstring, std::wstring>& stream_names,
    DWORD file_attributes_and_flags,
    PDOKAN_IO_SECURITY_CONTEXT security_context) {
  // When creating a new a alternated stream, we need to be sure
  // the main stream exist otherwise we create it.
  auto main_stream_name =
      memfs_helper::GetFileNameStreamLess(filename, stream_names);
  if (!fs_filenodes->find(main_stream_name)) {
    spdlog::info(L"create_main_stream: we create the maing stream {}", main_stream_name);
    auto n = fs_filenodes->add(std::make_shared<filenode>(main_stream_name, false,
                                   file_attributes_and_flags, security_context),
        {});
    if (n != STATUS_SUCCESS) return n;
  }
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
memfs_createfile(LPCWSTR filename, PDOKAN_IO_SECURITY_CONTEXT security_context,
                 ACCESS_MASK desiredaccess, ULONG fileattributes,
                 ULONG /*shareaccess*/, ULONG createdisposition,
                 ULONG createoptions, PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  ACCESS_MASK generic_desiredaccess;
  DWORD creation_disposition;
  DWORD file_attributes_and_flags;

  DokanMapKernelToUserCreateFileFlags(
      desiredaccess, fileattributes, createoptions, createdisposition,
      &generic_desiredaccess, &file_attributes_and_flags,
      &creation_disposition);

  auto filename_str = std::wstring(filename);
  memfs_helper::RemoveStreamType(filename_str);

  auto f = filenodes->find(filename_str);
  auto stream_names = memfs_helper::GetStreamNames(filename_str);

  spdlog::info(L"CreateFile: {} with node: {}", filename_str, (f != nullptr));

  // We only support filename length under 255.
  // See GetVolumeInformation - MaximumComponentLength
  if (stream_names.first.length() > 255) return STATUS_OBJECT_NAME_INVALID;

  // Windows will automatically try to create and access different system
  // directories.
  if (filename_str == L"\\System Volume Information" ||
      filename_str == L"\\$RECYCLE.BIN") {
    return STATUS_NO_SUCH_FILE;
  }

  if (f && f->is_directory) {
    if (createoptions & FILE_NON_DIRECTORY_FILE)
      return STATUS_FILE_IS_A_DIRECTORY;
    dokanfileinfo->IsDirectory = true;
  }

  // TODO Use AccessCheck to check security rights

  if (dokanfileinfo->IsDirectory) {
    spdlog::info(L"CreateFile: {} is a Directory", filename_str);

    if (creation_disposition == CREATE_NEW ||
        creation_disposition == OPEN_ALWAYS) {
      spdlog::info(L"CreateFile: {} create Directory", filename_str);
      // Cannot create a stream as directory.
      if (!stream_names.second.empty()) return STATUS_NOT_A_DIRECTORY;

      if (f) return STATUS_OBJECT_NAME_COLLISION;

      auto newfileNode = std::make_shared<filenode>(
          filename_str, true, FILE_ATTRIBUTE_DIRECTORY, security_context);
      return filenodes->add(newfileNode, stream_names);
    }

    if (f && !f->is_directory) return STATUS_NOT_A_DIRECTORY;
    if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;

    spdlog::info(L"CreateFile: {} open Directory", filename_str);
  } else {
    spdlog::info(L"CreateFile: {} is a File", filename_str);

    // Cannot overwrite an hidden or system file.
    if (f && (((!(file_attributes_and_flags & FILE_ATTRIBUTE_HIDDEN) &&
                (f->attributes & FILE_ATTRIBUTE_HIDDEN)) ||
               (!(file_attributes_and_flags & FILE_ATTRIBUTE_SYSTEM) &&
                (f->attributes & FILE_ATTRIBUTE_SYSTEM))) &&
              (creation_disposition == TRUNCATE_EXISTING ||
               creation_disposition == CREATE_ALWAYS)))
      return STATUS_ACCESS_DENIED;

    // Cannot delete a file with readonly attributes.
    if ((f && (f->attributes & FILE_ATTRIBUTE_READONLY) ||
         (file_attributes_and_flags & FILE_ATTRIBUTE_READONLY)) &&
        (file_attributes_and_flags & FILE_FLAG_DELETE_ON_CLOSE))
      return STATUS_CANNOT_DELETE;

    // Cannot open a readonly file for writing.
    if ((creation_disposition == OPEN_ALWAYS ||
         creation_disposition == OPEN_EXISTING) &&
        f && (f->attributes & FILE_ATTRIBUTE_READONLY) &&
        desiredaccess & FILE_WRITE_DATA)
      return STATUS_ACCESS_DENIED;

    // Cannot overwrite an existing read only file.
    // FILE_SUPERSEDE can as it replace and not overwrite.
    if ((creation_disposition == CREATE_NEW ||
         (creation_disposition == CREATE_ALWAYS &&
          createdisposition != FILE_SUPERSEDE) ||
         creation_disposition == TRUNCATE_EXISTING) &&
        f && (f->attributes & FILE_ATTRIBUTE_READONLY))
      return STATUS_ACCESS_DENIED;

    if (creation_disposition == CREATE_NEW ||
        creation_disposition == CREATE_ALWAYS ||
        creation_disposition == OPEN_ALWAYS ||
        creation_disposition == TRUNCATE_EXISTING) {
      // Combines the file attributes and flags specified by
      // dwFlagsAndAttributes with FILE_ATTRIBUTE_ARCHIVE.
      file_attributes_and_flags |= FILE_ATTRIBUTE_ARCHIVE;
      // We merge the attributes with the existing file attributes
      // except for FILE_SUPERSEDE.
      if (f && createdisposition != FILE_SUPERSEDE)
        file_attributes_and_flags |= f->attributes;
      // Remove non specific attributes.
      file_attributes_and_flags &= ~FILE_ATTRIBUTE_STRICTLY_SEQUENTIAL;
      // FILE_ATTRIBUTE_NORMAL is override if any other attribute is set.
      file_attributes_and_flags &= ~FILE_ATTRIBUTE_NORMAL;
    }

    switch (creation_disposition) {
      case CREATE_ALWAYS: {
        spdlog::info(L"CreateFile: {} CREATE_ALWAYS", filename_str);
        /*
         * Creates a new file, always.
         *
         * We handle FILE_SUPERSEDE here as it is converted to TRUNCATE_EXISTING
         * by DokanMapKernelToUserCreateFileFlags.
         */

        if (!stream_names.second.empty()) {
          // The createfile is a alternate stream,
          // we need to be sure main stream exist
          auto n =
              create_main_stream(filenodes, filename_str, stream_names,
                                 file_attributes_and_flags, security_context);
          if (n != STATUS_SUCCESS) return n;
        }

        auto n =
            filenodes->add(std::make_shared<filenode>(filename_str, false,
                                                      file_attributes_and_flags,
                                                      security_context),
                           stream_names);
        if (n != STATUS_SUCCESS) return n;

        /*
         * If the specified file exists and is writable, the function overwrites
         * the file, the function succeeds, and last-error code is set to
         * ERROR_ALREADY_EXISTS
         */
        if (f) return STATUS_OBJECT_NAME_COLLISION;
      } break;
      case CREATE_NEW: {
        spdlog::info(L"CreateFile: {} CREATE_NEW", filename_str);
        /*
         * Creates a new file, only if it does not already exist.
         */
        if (f) return STATUS_OBJECT_NAME_COLLISION;

        if (!stream_names.second.empty()) {
          // The createfile is a alternate stream,
          // we need to be sure main stream exist
          auto n =
              create_main_stream(filenodes, filename_str, stream_names,
                                 file_attributes_and_flags, security_context);
          if (n != STATUS_SUCCESS) return n;
        }

        auto n = filenodes->add(std::make_shared<filenode>(filename_str, false,
                                                      file_attributes_and_flags,
                                                      security_context),
                           stream_names);
        if (n != STATUS_SUCCESS) return n;
      } break;
      case OPEN_ALWAYS: {
        spdlog::info(L"CreateFile: {} OPEN_ALWAYS", filename_str);
        /*
         * Opens a file, always.
         */

        if (!f) {
          auto n = filenodes->add(std::make_shared<filenode>(
              filename_str, false, file_attributes_and_flags,
                                 security_context),
                             stream_names);
          if (n != STATUS_SUCCESS) return n;
        } else {
          if (desiredaccess & FILE_EXECUTE) {
            f->times.lastaccess = filetimes::get_currenttime();
          }
        }
      } break;
      case OPEN_EXISTING: {
        spdlog::info(L"CreateFile: {} OPEN_EXISTING", filename_str);
        /*
         * Opens a file or device, only if it exists.
         * If the specified file or device does not exist, the function fails
         * and the last-error code is set to ERROR_FILE_NOT_FOUND
         */
        if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;

        if (desiredaccess & FILE_EXECUTE) {
          f->times.lastaccess = filetimes::get_currenttime();
        }
      } break;
      case TRUNCATE_EXISTING: {
        spdlog::info(L"CreateFile: {} TRUNCATE_EXISTING", filename_str);
        /*
         * Opens a file and truncates it so that its size is zero bytes, only if
         * it exists. If the specified file does not exist, the function fails
         * and the last-error code is set to ERROR_FILE_NOT_FOUND
         */
        if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;

        f->set_endoffile(0);
        f->times.lastaccess = f->times.lastwrite = filetimes::get_currenttime();
        f->attributes = file_attributes_and_flags;
      } break;
      default:
        spdlog::info(L"CreateFile: {} Unknown CreationDisposition {}",
                     filename_str, creation_disposition);
        break;
    }
  }

  /*
   * CREATE_NEW && OPEN_ALWAYS
   * If the specified file exists, the function fails and the last-error code is
   * set to ERROR_FILE_EXISTS
   */
  if (f && (creation_disposition == CREATE_NEW ||
            creation_disposition == OPEN_ALWAYS))
    return STATUS_OBJECT_NAME_COLLISION;

  return STATUS_SUCCESS;
}

static void DOKAN_CALLBACK memfs_cleanup(LPCWSTR filename,
                                         PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  spdlog::info(L"Cleanup: {}", filename_str);
  if (dokanfileinfo->DeleteOnClose) {
    // Delete happens during cleanup and not in close event.
    spdlog::info(L"\tDeleteOnClose: {}", filename_str);
    filenodes->remove(filename_str);
  }
}

static void DOKAN_CALLBACK memfs_closeFile(LPCWSTR filename,
                                           PDOKAN_FILE_INFO /*dokanfileinfo*/) {
  auto filename_str = std::wstring(filename);
  // Here we should release all resources from the createfile context if we had.
  spdlog::info(L"CloseFile: {}", filename_str);
}

static NTSTATUS DOKAN_CALLBACK memfs_readfile(LPCWSTR filename, LPVOID buffer,
                                              DWORD bufferlength,
                                              LPDWORD readlength,
                                              LONGLONG offset,
                                              PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  spdlog::info(L"ReadFile: {}", filename_str);
  auto f = filenodes->find(filename_str);
  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;

  *readlength = f->read(buffer, bufferlength, offset);
  spdlog::info(L"\tBufferLength: {} offset: {} readlength: {}", bufferlength,
               offset, *readlength);
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK memfs_writefile(LPCWSTR filename, LPCVOID buffer,
                                               DWORD number_of_bytes_to_write,
                                               LPDWORD number_of_bytes_written,
                                               LONGLONG offset,
                                               PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  spdlog::info(L"WriteFile: {}", filename_str);
  auto f = filenodes->find(filename_str);
  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;

  auto file_size = f->get_filesize();

  // An Offset -1 is like the file was opened with FILE_APPEND_DATA
  // and we need to write at the end of the file.
  if (offset == -1) offset = file_size;

  if (dokanfileinfo->PagingIo) {
    // PagingIo cannot extend file size.
    // We return STATUS_SUCCESS when offset is beyond fileSize
    // and write the maximum we are allowed to.
    if (offset >= file_size) {
      spdlog::info(L"\tPagingIo Outside offset: {} FileSize: {}", offset,
                   file_size);
      *number_of_bytes_written = 0;
      return STATUS_SUCCESS;
    }

    if ((offset + number_of_bytes_to_write) > file_size) {
      // resize the write length to not go beyond file size.
      LONGLONG bytes = file_size - offset;
      if (bytes >> 32) {
        number_of_bytes_to_write = static_cast<DWORD>(bytes & 0xFFFFFFFFUL);
      } else {
        number_of_bytes_to_write = static_cast<DWORD>(bytes);
      }
    }
    spdlog::info(L"\tPagingIo number_of_bytes_to_write: {}",
                 number_of_bytes_to_write);
  }

  *number_of_bytes_written = f->write(buffer, number_of_bytes_to_write, offset);

  spdlog::info(
      L"\tNumberOfBytesToWrite {} offset: {} number_of_bytes_written: {}",
      number_of_bytes_to_write, offset, *number_of_bytes_written);
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
memfs_flushfilebuffers(LPCWSTR filename, PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  spdlog::info(L"FlushFileBuffers: {}", filename_str);
  auto f = filenodes->find(filename_str);
  // Nothing to flush, we directly write the content into our buffer.

  if (f->main_stream) f = f->main_stream;
  f->times.lastaccess = f->times.lastwrite = filetimes::get_currenttime();

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
memfs_getfileInformation(LPCWSTR filename, LPBY_HANDLE_FILE_INFORMATION buffer,
                         PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  spdlog::info(L"GetFileInformation: {}", filename_str);
  auto f = filenodes->find(filename_str);
  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;
  buffer->dwFileAttributes = f->attributes;
  memfs_helper::LlongToFileTime(f->times.creation, buffer->ftCreationTime);
  memfs_helper::LlongToFileTime(f->times.lastaccess, buffer->ftLastAccessTime);
  memfs_helper::LlongToFileTime(f->times.lastwrite, buffer->ftLastWriteTime);
  auto strLength = f->get_filesize();
  memfs_helper::LlongToDwLowHigh(strLength, buffer->nFileSizeLow,
                                 buffer->nFileSizeHigh);
  memfs_helper::LlongToDwLowHigh(f->fileindex, buffer->nFileIndexLow,
                                 buffer->nFileIndexHigh);
  // We do not track the number of links to the file so we return a fake value.
  buffer->nNumberOfLinks = 1;
  buffer->dwVolumeSerialNumber = g_volumserial;

  spdlog::info(
      L"GetFileInformation: {} Attributes: {:x} Times: Creation {:x} "
      L"LastAccess {:x} LastWrite {:x} FileSize {} NumberOfLinks {} "
      L"VolumeSerialNumber {:x}",
      filename_str, f->attributes, f->times.creation, f->times.lastaccess,
      f->times.lastwrite, strLength, buffer->nNumberOfLinks,
      buffer->dwVolumeSerialNumber);

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK memfs_findfiles(LPCWSTR filename,
                                               PFillFindData fill_finddata,
                                               PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  auto files = filenodes->list_folder(filename_str);
  WIN32_FIND_DATAW findData;
  spdlog::info(L"FindFiles: {}", filename_str);
  ZeroMemory(&findData, sizeof(WIN32_FIND_DATAW));
  for (const auto& f : files) {
    if (f->main_stream) continue; // Do not list File Streams
    const auto fileNodeName = memfs_helper::GetFileName(f->get_filename());
    if (fileNodeName.size() > MAX_PATH)
      continue;
    std::copy(fileNodeName.begin(), fileNodeName.end(),
              std::begin(findData.cFileName));
    findData.cFileName[fileNodeName.length()] = '\0';
    findData.dwFileAttributes = f->attributes;
    memfs_helper::LlongToFileTime(f->times.creation, findData.ftCreationTime);
    memfs_helper::LlongToFileTime(f->times.lastaccess,
                                  findData.ftLastAccessTime);
    memfs_helper::LlongToFileTime(f->times.lastwrite, findData.ftLastWriteTime);
    auto file_size = f->get_filesize();
    memfs_helper::LlongToDwLowHigh(file_size, findData.nFileSizeLow,
                                   findData.nFileSizeHigh);
    spdlog::info(
        L"FindFiles: {} fileNode: {} Attributes: {} Times: Creation {} "
        L"LastAccess {} LastWrite {} FileSize {}",
        filename_str, fileNodeName, findData.dwFileAttributes,
        f->times.creation, f->times.lastaccess, f->times.lastwrite, file_size);
    fill_finddata(&findData, dokanfileinfo);
  }
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK memfs_setfileattributes(
    LPCWSTR filename, DWORD fileattributes, PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  auto f = filenodes->find(filename_str);
  spdlog::info(L"SetFileAttributes: {} fileattributes {}", filename_str,
               fileattributes);
  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;

  // from https://docs.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileattributesw
  DWORD const attributes_allowed_to_set =
      FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL |
      FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_OFFLINE |
      FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM |
      FILE_ATTRIBUTE_TEMPORARY;

  fileattributes &= attributes_allowed_to_set;

  DWORD new_file_attributes =
      (f->attributes & ~attributes_allowed_to_set) | fileattributes;

  // FILE_ATTRIBUTE_NORMAL is overriden if any other attribute is set
  if ((new_file_attributes & FILE_ATTRIBUTE_NORMAL) &&
      (new_file_attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_NORMAL)))
    new_file_attributes &= ~static_cast<DWORD>(FILE_ATTRIBUTE_NORMAL);

  f->attributes = new_file_attributes;
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
memfs_setfiletime(LPCWSTR filename, CONST FILETIME* creationtime,
                  CONST FILETIME* lastaccesstime, CONST FILETIME* lastwritetime,
                  PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  auto f = filenodes->find(filename_str);
  spdlog::info(L"SetFileTime: {}", filename_str);
  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;
  if (creationtime && !filetimes::empty(creationtime))
    f->times.creation = memfs_helper::FileTimeToLlong(*creationtime);
  if (lastaccesstime && !filetimes::empty(lastaccesstime))
    f->times.lastaccess = memfs_helper::FileTimeToLlong(*lastaccesstime);
  if (lastwritetime && !filetimes::empty(lastwritetime))
    f->times.lastwrite = memfs_helper::FileTimeToLlong(*lastwritetime);
  // We should update Change Time here but dokan use lastwritetime for both.
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
memfs_deletefile(LPCWSTR filename, PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  auto f = filenodes->find(filename_str);
  spdlog::info(L"DeleteFile: {}", filename_str);

  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;

  if (f->is_directory) return STATUS_ACCESS_DENIED;

  // Here prepare and check if the file can be deleted
  // or if delete is canceled when dokanfileinfo->DeleteOnClose false

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
memfs_deletedirectory(LPCWSTR filename, PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  spdlog::info(L"DeleteDirectory: {}", filename_str);

  if (filenodes->list_folder(filename_str).size())
    return STATUS_DIRECTORY_NOT_EMPTY;

  // Here prepare and check if the directory can be deleted
  // or if delete is canceled when dokanfileinfo->DeleteOnClose false

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK memfs_movefile(LPCWSTR filename,
                                              LPCWSTR new_filename,
                                              BOOL replace_if_existing,
                                              PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  auto new_filename_str = std::wstring(new_filename);
  spdlog::info(L"MoveFile: {} to {}", filename_str, new_filename_str);
  memfs_helper::RemoveStreamType(new_filename_str);
  auto new_stream_names = memfs_helper::GetStreamNames(new_filename_str);
  if (new_stream_names.first.empty()) {
    // new_filename is a stream name :<stream name>:<stream type>
    // We removed the stream type and now need to concat the filename and the
    // new stream name
    auto stream_names = memfs_helper::GetStreamNames(filename_str);
    new_filename_str =
        memfs_helper::GetFileNameStreamLess(filename, stream_names) +
                       L":" + new_stream_names.second;
  }
  spdlog::info(L"MoveFile: after {} to {}", filename_str, new_filename_str);
  return filenodes->move(filename_str, new_filename_str, replace_if_existing);
}

static NTSTATUS DOKAN_CALLBACK memfs_setendoffile(
    LPCWSTR filename, LONGLONG ByteOffset, PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  spdlog::info(L"SetEndOfFile: {} ByteOffset {}", filename_str, ByteOffset);
  auto f = filenodes->find(filename_str);

  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;
  f->set_endoffile(ByteOffset);
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK memfs_setallocationsize(
    LPCWSTR filename, LONGLONG alloc_size, PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  spdlog::info(L"SetAllocationSize: {} AllocSize {}", filename_str, alloc_size);
  auto f = filenodes->find(filename_str);

  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;
  f->set_endoffile(alloc_size);
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK memfs_lockfile(LPCWSTR filename,
                                              LONGLONG byte_offset,
                                              LONGLONG length,
                                              PDOKAN_FILE_INFO dokanfileinfo) {
  auto filename_str = std::wstring(filename);
  spdlog::info(L"LockFile: {} ByteOffset {} Length {}", filename_str,
               byte_offset, length);
  return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS DOKAN_CALLBACK
memfs_unlockfile(LPCWSTR filename, LONGLONG byte_offset, LONGLONG length,
                 PDOKAN_FILE_INFO dokanfileinfo) {
  auto filename_str = std::wstring(filename);
  spdlog::info(L"UnlockFile: {} ByteOffset {} Length {}", filename_str,
               byte_offset, length);
  return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS DOKAN_CALLBACK memfs_getdiskfreespace(
    PULONGLONG free_bytes_available, PULONGLONG total_number_of_bytes,
    PULONGLONG total_number_of_free_bytes, PDOKAN_FILE_INFO dokanfileinfo) {
  spdlog::info(L"GetDiskFreeSpace");
  *free_bytes_available = (ULONGLONG)(512 * 1024 * 1024);
  *total_number_of_bytes = MAXLONGLONG;
  *total_number_of_free_bytes = MAXLONGLONG;
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK memfs_getvolumeinformation(
    LPWSTR volumename_buffer, DWORD volumename_size,
    LPDWORD volume_serialnumber, LPDWORD maximum_component_length,
    LPDWORD filesystem_flags, LPWSTR filesystem_name_buffer,
    DWORD filesystem_name_size, PDOKAN_FILE_INFO /*dokanfileinfo*/) {
  spdlog::info(L"GetVolumeInformation");
  wcscpy_s(volumename_buffer, volumename_size, L"Dokan MemFS");
  *volume_serialnumber = g_volumserial;
  *maximum_component_length = 255;
  *filesystem_flags = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
                      FILE_SUPPORTS_REMOTE_STORAGE | FILE_UNICODE_ON_DISK |
                      FILE_NAMED_STREAMS;

  wcscpy_s(filesystem_name_buffer, filesystem_name_size, L"NTFS");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
memfs_mounted(LPCWSTR MountPoint, PDOKAN_FILE_INFO dokanfileinfo) {
  spdlog::info(L"Mounted as {}", MountPoint);
  WCHAR *mount_point =
      (reinterpret_cast<memfs *>(dokanfileinfo->DokanOptions->GlobalContext))
          ->mount_point;
  wcscpy_s(mount_point, sizeof(WCHAR) * MAX_PATH, MountPoint);
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
memfs_unmounted(PDOKAN_FILE_INFO /*dokanfileinfo*/) {
  spdlog::info(L"Unmounted");
  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK memfs_getfilesecurity(
    LPCWSTR filename, PSECURITY_INFORMATION security_information,
    PSECURITY_DESCRIPTOR security_descriptor, ULONG bufferlength,
    PULONG length_needed, PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  spdlog::info(L"GetFileSecurity: {}", filename_str);
  auto f = filenodes->find(filename_str);

  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;

  std::shared_lock lockFile(f->security);

  // This will make dokan library return a default security descriptor
  if (!f->security.descriptor) return STATUS_NOT_IMPLEMENTED;

  // We have a Security Descriptor but we need to extract only informations
  // requested 1 - Convert the Security Descriptor to SDDL string with the
  // informations requested
  LPTSTR pStringBuffer = NULL;
  if (!ConvertSecurityDescriptorToStringSecurityDescriptor(
          f->security.descriptor.get(), SDDL_REVISION_1, *security_information,
          &pStringBuffer, NULL)) {
    return STATUS_NOT_IMPLEMENTED;
  }

  // 2 - Convert the SDDL string back to Security Descriptor
  PSECURITY_DESCRIPTOR SecurityDescriptorTmp = NULL;
  ULONG Size = 0;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptor(
          pStringBuffer, SDDL_REVISION_1, &SecurityDescriptorTmp, &Size)) {
    LocalFree(pStringBuffer);
    return STATUS_NOT_IMPLEMENTED;
  }
  LocalFree(pStringBuffer);

  *length_needed = Size;
  if (Size > bufferlength) {
    LocalFree(SecurityDescriptorTmp);
    return STATUS_BUFFER_OVERFLOW;
  }

  // 3 - Copy the new SecurityDescriptor to destination
  memcpy(security_descriptor, SecurityDescriptorTmp, Size);
  LocalFree(SecurityDescriptorTmp);

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK memfs_setfilesecurity(
    LPCWSTR filename, PSECURITY_INFORMATION security_information,
    PSECURITY_DESCRIPTOR security_descriptor, ULONG /*bufferlength*/,
    PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  spdlog::info(L"SetFileSecurity: {}", filename_str);
  static GENERIC_MAPPING memfs_mapping = {FILE_GENERIC_READ, FILE_GENERIC_WRITE,
                                          FILE_GENERIC_EXECUTE,
                                          FILE_ALL_ACCESS};
  auto f = filenodes->find(filename_str);

  if (!f) return STATUS_OBJECT_NAME_NOT_FOUND;

  std::unique_lock securityLock(f->security);

  // SetPrivateObjectSecurity - ObjectsSecurityDescriptor
  // The memory for the security descriptor must be allocated from the process
  // heap (GetProcessHeap) with the HeapAlloc function.
  // https://devblogs.microsoft.com/oldnewthing/20170727-00/?p=96705
  HANDLE pHeap = GetProcessHeap();
  PSECURITY_DESCRIPTOR heapSecurityDescriptor =
      HeapAlloc(pHeap, 0, f->security.descriptor_size);
  if (!heapSecurityDescriptor) return STATUS_INSUFFICIENT_RESOURCES;
  // Copy our current descriptor into heap memory
  memcpy(heapSecurityDescriptor, f->security.descriptor.get(),
         f->security.descriptor_size);

  if (!SetPrivateObjectSecurity(*security_information, security_descriptor,
                                &heapSecurityDescriptor, &memfs_mapping, 0)) {
    HeapFree(pHeap, 0, heapSecurityDescriptor);
    return DokanNtStatusFromWin32(GetLastError());
  }

  f->security.SetDescriptor(heapSecurityDescriptor);
  HeapFree(pHeap, 0, heapSecurityDescriptor);

  return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK
memfs_findstreams(LPCWSTR filename, PFillFindStreamData fill_findstreamdata,
                  PVOID findstreamcontext, PDOKAN_FILE_INFO dokanfileinfo) {
  auto filenodes = GET_FS_INSTANCE;
  auto filename_str = std::wstring(filename);
  spdlog::info(L"FindStreams: {}", filename_str);
  auto f = filenodes->find(filename_str);

  if (!f)
    return STATUS_OBJECT_NAME_NOT_FOUND;

  auto streams = f->get_streams();

  WIN32_FIND_STREAM_DATA stream_data;
  ZeroMemory(&stream_data, sizeof(WIN32_FIND_STREAM_DATA));

  if (!f->is_directory) {
    // Add the main stream name - \foo::$DATA by returning ::$DATA
    std::copy(memfs_helper::DataStreamNameStr.begin(),
              memfs_helper::DataStreamNameStr.end(),
              std::begin(stream_data.cStreamName) + 1);
    stream_data.cStreamName[0] = ':';
    stream_data.cStreamName[memfs_helper::DataStreamNameStr.length() + 1] =
        L'\0';
    stream_data.StreamSize.QuadPart = f->get_filesize();
    if (!fill_findstreamdata(&stream_data, findstreamcontext)) {
      return STATUS_BUFFER_OVERFLOW;
    }
  } else if (streams.empty()) {
    // The node is a directory without any alternate streams
    return STATUS_END_OF_FILE;
  }

  // Add the alternated stream attached
  // for \foo:bar we need to return in the form of bar:$DATA
  for (const auto &stream : streams) {
    auto stream_names = memfs_helper::GetStreamNames(stream.first);
    if (stream_names.second.length() +
            memfs_helper::DataStreamNameStr.length() + 1 >
        sizeof(stream_data.cStreamName))
      continue;
    // Copy the filename foo
    std::copy(stream_names.second.begin(), stream_names.second.end(),
              std::begin(stream_data.cStreamName) + 1);
    // Concat :$DATA
    std::copy(memfs_helper::DataStreamNameStr.begin(),
              memfs_helper::DataStreamNameStr.end(),
              std::begin(stream_data.cStreamName) +
                  stream_names.second.length() + 1);
    stream_data.cStreamName[0] = ':';
    stream_data.cStreamName[stream_names.second.length() +
                            memfs_helper::DataStreamNameStr.length() + 1] =
        L'\0';
    stream_data.StreamSize.QuadPart = stream.second->get_filesize();
    spdlog::info(L"FindStreams: {} StreamName: {} Size: {:x}", filename_str,
                 stream_names.second, stream_data.StreamSize.QuadPart);
    if (!fill_findstreamdata(&stream_data, findstreamcontext)) {
      return STATUS_BUFFER_OVERFLOW;
    }
  }
  return STATUS_SUCCESS;
}

DOKAN_OPERATIONS memfs_operations = {memfs_createfile,
                                     memfs_cleanup,
                                     memfs_closeFile,
                                     memfs_readfile,
                                     memfs_writefile,
                                     memfs_flushfilebuffers,
                                     memfs_getfileInformation,
                                     memfs_findfiles,
                                     nullptr,  // FindFilesWithPattern
                                     memfs_setfileattributes,
                                     memfs_setfiletime,
                                     memfs_deletefile,
                                     memfs_deletedirectory,
                                     memfs_movefile,
                                     memfs_setendoffile,
                                     memfs_setallocationsize,
                                     memfs_lockfile,
                                     memfs_unlockfile,
                                     memfs_getdiskfreespace,
                                     memfs_getvolumeinformation,
                                     memfs_mounted,
                                     memfs_unmounted,
                                     memfs_getfilesecurity,
                                     memfs_setfilesecurity,
                                     memfs_findstreams};
}  // namespace memfs
