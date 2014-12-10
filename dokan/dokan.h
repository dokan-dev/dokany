/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2008 Hiroki Asakawa info@dokan-dev.net

  http://dokan-dev.net/en

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program. If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef _DOKAN_H_
#define _DOKAN_H_

#define DOKAN_DRIVER_NAME	L"dokan.sys"

#ifndef _M_X64	
  #ifdef _EXPORTING
	#define DOKANAPI __declspec(dllimport) __stdcall
  #else
	#define DOKANAPI __declspec(dllexport) __stdcall
  #endif
#else
  #define DOKANAPI
#endif

#define DOKAN_CALLBACK __stdcall

#ifdef __cplusplus
extern "C" {
#endif

// The current Dokan version (ver 0.6.0). Please set this constant on DokanOptions->Version.
#define DOKAN_VERSION		600

#define DOKAN_OPTION_DEBUG		1 // ouput debug message
#define DOKAN_OPTION_STDERR		2 // ouput debug message to stderr
#define DOKAN_OPTION_ALT_STREAM	4 // use alternate stream
#define DOKAN_OPTION_KEEP_ALIVE	8 // use auto unmount
#define DOKAN_OPTION_NETWORK	16 // use network drive, you need to install Dokan network provider.
#define DOKAN_OPTION_REMOVABLE	32 // use removable drive

typedef struct _DOKAN_OPTIONS {
	USHORT	Version; // Supported Dokan Version, ex. "530" (Dokan ver 0.5.3)
	USHORT	ThreadCount; // number of threads to be used
	ULONG	Options;	 // combination of DOKAN_OPTIONS_*
	ULONG64	GlobalContext; // FileSystem can use this variable
	LPCWSTR	MountPoint; //  mount point "M:\" (drive letter) or "C:\mount\dokan" (path in NTFS)
} DOKAN_OPTIONS, *PDOKAN_OPTIONS;

typedef struct _DOKAN_FILE_INFO {
	ULONG64	Context;      // FileSystem can use this variable
	ULONG64	DokanContext; // Don't touch this
	PDOKAN_OPTIONS DokanOptions; // A pointer to DOKAN_OPTIONS which was  passed to DokanMain.
	ULONG	ProcessId;    // process id for the thread that originally requested a given I/O operation
	UCHAR	IsDirectory;  // requesting a directory file
	UCHAR	DeleteOnClose; // Delete on when "cleanup" is called
	UCHAR	PagingIo;	// Read or write is paging IO.
	UCHAR	SynchronousIo;  // Read or write is synchronous IO.
	UCHAR	Nocache;
	UCHAR	WriteToEndOfFile; //  If true, write to the current end of file instead of Offset parameter.

} DOKAN_FILE_INFO, *PDOKAN_FILE_INFO;


// FillFileData
//   add an entry in FindFiles
//   return 1 if buffer is full, otherwise 0
//   (currently never return 1)
typedef int (WINAPI *PFillFindData) (PWIN32_FIND_DATAW, PDOKAN_FILE_INFO);

typedef struct _DOKAN_OPERATIONS {

	// When an error occurs, return negative value.
	// Usually you should return GetLastError() * -1.


	// CreateFile
	//   If file is a directory, CreateFile (not OpenDirectory) may be called.
	//   In this case, CreateFile should return 0 when that directory can be opened.
	//   You should set TRUE on DokanFileInfo->IsDirectory when file is a directory.
	//   When CreationDisposition is CREATE_ALWAYS or OPEN_ALWAYS and a file already exists,
	//   you should return ERROR_ALREADY_EXISTS(183) (not negative value)
	int (DOKAN_CALLBACK *CreateFile) (
		LPCWSTR,      // FileName
		DWORD,        // DesiredAccess
		DWORD,        // ShareMode
		DWORD,        // CreationDisposition
		DWORD,        // FlagsAndAttributes
		PDOKAN_FILE_INFO);

	int (DOKAN_CALLBACK *OpenDirectory) (
		LPCWSTR,				// FileName
		PDOKAN_FILE_INFO);

	int (DOKAN_CALLBACK *CreateDirectory) (
		LPCWSTR,				// FileName
		PDOKAN_FILE_INFO);

	// When FileInfo->DeleteOnClose is true, you must delete the file in Cleanup.
	int (DOKAN_CALLBACK *Cleanup) (
		LPCWSTR,      // FileName
		PDOKAN_FILE_INFO);

	int (DOKAN_CALLBACK *CloseFile) (
		LPCWSTR,      // FileName
		PDOKAN_FILE_INFO);

	int (DOKAN_CALLBACK *ReadFile) (
		LPCWSTR,  // FileName
		LPVOID,   // Buffer
		DWORD,    // NumberOfBytesToRead
		LPDWORD,  // NumberOfBytesRead
		LONGLONG, // Offset
		PDOKAN_FILE_INFO);
	

	int (DOKAN_CALLBACK *WriteFile) (
		LPCWSTR,  // FileName
		LPCVOID,  // Buffer
		DWORD,    // NumberOfBytesToWrite
		LPDWORD,  // NumberOfBytesWritten
		LONGLONG, // Offset
		PDOKAN_FILE_INFO);


	int (DOKAN_CALLBACK *FlushFileBuffers) (
		LPCWSTR, // FileName
		PDOKAN_FILE_INFO);


	int (DOKAN_CALLBACK *GetFileInformation) (
		LPCWSTR,          // FileName
		LPBY_HANDLE_FILE_INFORMATION, // Buffer
		PDOKAN_FILE_INFO);
	

	int (DOKAN_CALLBACK *FindFiles) (
		LPCWSTR,			// PathName
		PFillFindData,		// call this function with PWIN32_FIND_DATAW
		PDOKAN_FILE_INFO);  //  (see PFillFindData definition)


	// You should implement either FindFiles or FindFilesWithPattern
	int (DOKAN_CALLBACK *FindFilesWithPattern) (
		LPCWSTR,			// PathName
		LPCWSTR,			// SearchPattern
		PFillFindData,		// call this function with PWIN32_FIND_DATAW
		PDOKAN_FILE_INFO);


	int (DOKAN_CALLBACK *SetFileAttributes) (
		LPCWSTR, // FileName
		DWORD,   // FileAttributes
		PDOKAN_FILE_INFO);


	int (DOKAN_CALLBACK *SetFileTime) (
		LPCWSTR,		// FileName
		CONST FILETIME*, // CreationTime
		CONST FILETIME*, // LastAccessTime
		CONST FILETIME*, // LastWriteTime
		PDOKAN_FILE_INFO);


	// You should not delete file on DeleteFile or DeleteDirectory.
	// When DeleteFile or DeleteDirectory, you must check whether
	// you can delete the file or not, and return 0 (when you can delete it)
	// or appropriate error codes such as -ERROR_DIR_NOT_EMPTY,
	// -ERROR_SHARING_VIOLATION.
	// When you return 0 (ERROR_SUCCESS), you get Cleanup with
	// FileInfo->DeleteOnClose set TRUE and you have to delete the
	// file in Close.
	int (DOKAN_CALLBACK *DeleteFile) (
		LPCWSTR, // FileName
		PDOKAN_FILE_INFO);

	int (DOKAN_CALLBACK *DeleteDirectory) ( 
		LPCWSTR, // FileName
		PDOKAN_FILE_INFO);


	int (DOKAN_CALLBACK *MoveFile) (
		LPCWSTR, // ExistingFileName
		LPCWSTR, // NewFileName
		BOOL,	// ReplaceExisiting
		PDOKAN_FILE_INFO);


	int (DOKAN_CALLBACK *SetEndOfFile) (
		LPCWSTR,  // FileName
		LONGLONG, // Length
		PDOKAN_FILE_INFO);


	int (DOKAN_CALLBACK *SetAllocationSize) (
		LPCWSTR,  // FileName
		LONGLONG, // Length
		PDOKAN_FILE_INFO);


	int (DOKAN_CALLBACK *LockFile) (
		LPCWSTR, // FileName
		LONGLONG, // ByteOffset
		LONGLONG, // Length
		PDOKAN_FILE_INFO);


	int (DOKAN_CALLBACK *UnlockFile) (
		LPCWSTR, // FileName
		LONGLONG,// ByteOffset
		LONGLONG,// Length
		PDOKAN_FILE_INFO);


	// Neither GetDiskFreeSpace nor GetVolumeInformation
	// save the DokanFileContext->Context.
	// Before these methods are called, CreateFile may not be called.
	// (ditto CloseFile and Cleanup)

	// see Win32 API GetDiskFreeSpaceEx
	int (DOKAN_CALLBACK *GetDiskFreeSpace) (
		PULONGLONG, // FreeBytesAvailable
		PULONGLONG, // TotalNumberOfBytes
		PULONGLONG, // TotalNumberOfFreeBytes
		PDOKAN_FILE_INFO);


	// see Win32 API GetVolumeInformation
	int (DOKAN_CALLBACK *GetVolumeInformation) (
		LPWSTR, // VolumeNameBuffer
		DWORD,	// VolumeNameSize in num of chars
		LPDWORD,// VolumeSerialNumber
		LPDWORD,// MaximumComponentLength in num of chars
		LPDWORD,// FileSystemFlags
		LPWSTR,	// FileSystemNameBuffer
		DWORD,	// FileSystemNameSize in num of chars
		PDOKAN_FILE_INFO);


	int (DOKAN_CALLBACK *Unmount) (
		PDOKAN_FILE_INFO);


	// Suported since 0.6.0. You must specify the version at DOKAN_OPTIONS.Version.
	int (DOKAN_CALLBACK *GetFileSecurity) (
		LPCWSTR, // FileName
		PSECURITY_INFORMATION, // A pointer to SECURITY_INFORMATION value being requested
		PSECURITY_DESCRIPTOR, // A pointer to SECURITY_DESCRIPTOR buffer to be filled
		ULONG, // length of Security descriptor buffer
		PULONG, // LengthNeeded
		PDOKAN_FILE_INFO);

	int (DOKAN_CALLBACK *SetFileSecurity) (
		LPCWSTR, // FileName
		PSECURITY_INFORMATION,
		PSECURITY_DESCRIPTOR, // SecurityDescriptor
		ULONG, // SecurityDescriptor length
		PDOKAN_FILE_INFO);


} DOKAN_OPERATIONS, *PDOKAN_OPERATIONS;



/* DokanMain returns error codes */
#define DOKAN_SUCCESS				 0
#define DOKAN_ERROR					-1 /* General Error */
#define DOKAN_DRIVE_LETTER_ERROR	-2 /* Bad Drive letter */
#define DOKAN_DRIVER_INSTALL_ERROR	-3 /* Can't install driver */
#define DOKAN_START_ERROR			-4 /* Driver something wrong */
#define DOKAN_MOUNT_ERROR			-5 /* Can't assign a drive letter or mount point */
#define DOKAN_MOUNT_POINT_ERROR		-6 /* Mount point is invalid */

int DOKANAPI
DokanMain(
	PDOKAN_OPTIONS	DokanOptions,
	PDOKAN_OPERATIONS DokanOperations);


BOOL DOKANAPI
DokanUnmount(
	WCHAR	DriveLetter);

BOOL DOKANAPI
DokanRemoveMountPoint(
	LPCWSTR MountPoint);


// DokanIsNameInExpression
//   check whether Name can match Expression
//   Expression can contain wildcard characters (? and *)
BOOL DOKANAPI
DokanIsNameInExpression(
	LPCWSTR		Expression,		// matching pattern
	LPCWSTR		Name,			// file name
	BOOL		IgnoreCase);


ULONG DOKANAPI
DokanVersion();

ULONG DOKANAPI
DokanDriverVersion();

// DokanResetTimeout
//   extends the time out of the current IO operation in driver.
BOOL DOKANAPI
DokanResetTimeout(
	ULONG				Timeout,	// timeout in millisecond
	PDOKAN_FILE_INFO	DokanFileInfo);

// Get the handle to Access Token
// This method needs be called in CreateFile, OpenDirectory or CreateDirectly callback.
// The caller must call CloseHandle for the returned handle.
HANDLE DOKANAPI
DokanOpenRequestorToken(
	PDOKAN_FILE_INFO	DokanFileInfo);

#ifdef __cplusplus
}
#endif


#endif // _DOKAN_H_
