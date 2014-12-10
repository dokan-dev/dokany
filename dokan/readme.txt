
  Dokan Libary

  Copyright(c) Hiroki Asakawa http://dokan-dev.net/en



What is Dokan Library
=====================

When you want to create a new file system on Windows, for example to
improve FAT or NTFS, you need to develope a file system
driver. Developing a device driver that works in kernel mode on
windows is extremley difficult.By using Dokan library, you can create
your own file systems very easily without writing device driver. Dokan
Library is similar to FUSE(Linux user mode file system) but works on
Windows.


Licensing
=========

Dokan library contains LGPL, MIT licensed programs.

- user-mode library (dokan.dll)  LGPL
- driver (dokan.sys)             LGPL
- control program (dokanctl.exe) MIT
- mount service (mouter.exe)     MIT
- samples (mirror.c)             MIT

For detals, please check license files.
LGPL license.lgpl.txt
GPL  license.gpl.txt
MIT  license.mit.txt

You can obtain source files from http://dokan-dev.net/en/download


Environment
===========

Dokan Library works on Windowx XP,2003,Vista,2008,7 x86 and Windows
2003,Vista,2008,7 x64.


How it works
============

Dokan library contains a user mode DLL (dokan.dll) and a kernel mode
file system driver (dokan.sys). Once Dokan file system driver is
installed, you can create file systems which can be seen as normal
file systems in Windows. The application that creates file systems
using Dokan library is called File system application. File operation
requests from user programs (e.g., CreateFile, ReadFile, WriteFile,
...) will be sent to the Windows I/O subsystem (runs in kernel mode)
which will subsequently forward the requests to the Dokan file system
driver (dokan.sys). By using functions provided by the Dokan user mode
library (dokan.dll), file system applications are able to register
callback functions to the file system driver. The file system driver
will invoke these callback routines in order to response to the
requests it received. The results of the callback routines will be
sent back to the user program. For example, when Windows Explorer
requests to open a directory, the OpenDirectory request will be sent
to Dokan file system driver and the driver will invoke the
OpenDirectory callback provided by the file system application. The
results of this routine are sent back to Windows Explorer as the
response to the OpenDirectory request. Therefore, the Dokan file
system driver acts as a proxy between user programs and file system
applications. The advantage of this approach is that it allows
programmers to develop file systems in user mode which is safe and
easy to debug.


Components of the Library and installation
==========================================

When the installer executes, it will install Dokan file system driver
(dokan.sys), register Dokan mount service (mouter.exe) and several
libraries. The detailed list of files installed is as follows:

SystemFolder\dokan.dll
   Dokan user mode library

SystemFolder\drivers\dokan.sys
   Dokan File System Driver

ProgramFilesFolder\Dokan\DokanLibrary\mounter.exe
   Dokan mouter service

ProgramFilesFolder\Dokan\DokanLibrary\dokanctl.exe
   Dokan control program

ProgramFilesFolder\Dokan\DokanLibrary\dokan.lib
   Dokan import library

ProgramFilesFolder\Dokan\DokanLibrary\dokan.h
   Dokan library header

ProgramFilesFolder\Dokan\DokanLibrary\readme.txt
   this file

You can use Add/Remove programs in Control Panel to uninstall Dokan.
It is required to restart your computer after uninstallation.


How to create your file systems
===============================

To create file system, an application needs to implement functions in
DOKAN_OPERATIONS structure (declared in dokan.h). Once implemented,
you can invoke DokanMain function with DOKAN_OPERATIONS as parameter
in order to mount the file system. The semantics of functions in
DOKAN_OPERATIONS is just similar to Windows APIs with the same
name. The parameters for these functions are therefore the same as for
the counterpart Windows APIs. These functions are called from many
threads so they need to be thread-safe, otherwise many problems may
occur.

These functions are typically invoked in this sequence:

1. CreateFile(OpenDirectory, OpenDirectory)
2. Other functions
3. Cleanup
4. CloseFile

Before file access operations (listing directory, reading file
attributes, ...), file creation functions (OpenDirectory, CreateFile,
...) are always invoked. On other hand, the function Cleanup always
get called by the Dokan file system driver when the file is closed by
the CloseFile Windows API.  Each function should return 0 when the
operation succeeded, otherwise it should return a negative value
represents error code. The error codes are negated Windows System
Error Codes. For examaple, when CreateFile can't open a file, you
should return -2( -1 * ERROR_FILE_NOT_FOUND).

The last parameter of each function is a DOKAN_FILE_INFO structure :

   typedef struct _DOKAN_FILE_INFO {

       ULONG64 Context;
       ULONG64 DokanContext;
       ULONG   ProcessId;
       BOOL    IsDirectory;

   } DOKAN_FILE_INFO, *PDOKAN_FILE_INFO;

Each file handle from user mode is associated with a DOKAN_FILE_INFO
struct. Hence, the content of the struct does not change if the same
file handle is used. The struct is created when the file is opened by
CreateFile system call and destroyed when the file is closed (by
CloseFile system call).  The meaning of each field in the struct is as
follows:

  Context : a specific value assigned by the file system
  application. File system application can freely use this variable to
  store values that are constant in a file access session (the period
  from CreateFile to CloseFile) such as file handle, etc.

  DokanContext : reserved. Used by Dokan library.

  ProcessId : Process ID of the process that opened the file.

  IsDirectory : determine if the opened file is a directory, see
  exceptions bellow.


   int (*CreateFile) (
       LPCWSTR,      // FileName
       DWORD,        // DesiredAccess
       DWORD,        // ShareMode
       DWORD,        // CreationDisposition
       DWORD,        // FlagsAndAttributes
       PDOKAN_FILE_INFO);

   int (*OpenDirectory) (
       LPCWSTR,          // FileName
       PDOKAN_FILE_INFO);

   int (*CreateDirectory) (
       LPCWSTR,          // FileName
       PDOKAN_FILE_INFO);

When the variable IsDirectory is set to TRUE, the file under the
operation is a directory. When it is FALSE, the file system
application programmers are required to set the variable to TRUE if
the current operation acts on a directory. If the value is FALSE and
the current operation is not acting on a directory, the programmers
should not change the variable. Note that setting the variable to TRUE
when a directory is accessed is very important for the Dokan
library. If it is not correctly set, the library does not know the
operation is acting on a directory and many problems may occur.
CreateFile should return ERROR_ALREADY_EXISTS (183) when the
CreationDisposition is CREATE_ALWAYS or OPEN_ALWAYS and the file under
question has already existed.

   int (*Cleanup) (
       LPCWSTR,      // FileName
       PDOKAN_FILE_INFO);

   int (*CloseFile) (
       LPCWSTR,      // FileName
       PDOKAN_FILE_INFO);

Cleanup is invoked when the function CloseHandle in Windows API is
executed. If the file system application stored file handle in the
Context variable when the function CreateFile is invoked, this should
be closed in the Cleanup function, not in CloseFile function. If the
user application calls CloseHandle and subsequently open the same
file, the CloseFile function of the file system application may not be
invoked before the CreateFile API is called. This may cause sharing
violation error.  Note: when user uses memory mapped file, WriteFile
or ReadFile function may be invoked after Cleanup in order to complete
the I/O operations. The file system application should also properly
work in this case.

   int (*FindFiles) (
       LPCWSTR,           // PathName
       PFillFindData,     // call this function with PWIN32_FIND_DATAW
       PDOKAN_FILE_INFO); //  (see PFillFindData definition)


   // You should implement either FindFires or FindFilesWithPattern
   int (*FindFilesWithPattern) (
       LPCWSTR,           // PathName
       LPCWSTR,           // SearchPattern
       PFillFindData,     // call this function with PWIN32_FIND_DATAW
       PDOKAN_FILE_INFO);

FindFiles or FindFilesWithPattern is called in order to response to
directory listing requests. You should implement either FielFiles or
FileFilesWithPttern.  For each directory entry, file system
application should call the function FillFindData (passed as a
function pointer to FindFiles, FindFilesWithPattern) with the
WIN32_FIND_DATAW structure filled with directory information:
FillFindData( &win32FindDataw, DokanFileInfo ).  It is the
responsibility of file systems to process wildcard patterns because
shells in Windows are not designed to work properly with pattern
matching. When file system application provides FindFiles, the
wildcard patterns are automatically processed by the Dokan
library. You can control wildcard matching by implementing
FindFilesWithPattern function.  The function DokanIsNameInExpression
exported by the Dokan library (dokan.dll) can be used to process
wildcard matching.


Mounting
========

   #define DOKAN_OPTION_DEBUG       1 // ouput debug message
   #define DOKAN_OPTION_STDERR      2 // ouput debug message to stderr
   #define DOKAN_OPTION_ALT_STREAM  4 // use alternate stream
   #define DOKAN_OPTION_KEEP_ALIVE  8 // use auto unmount
   #define DOKAN_OPTION_NETWORK    16 // use network drive,
                                      //you need to install Dokan network provider.
   #define DOKAN_OPTION_REMOVABLE  32 // use removable drive

   typedef struct _DOKAN_OPTIONS {
       USHORT  Version;  // Supported Dokan Version, ex. "530" (Dokan ver 0.5.3)
       ULONG   ThreadCount;  // number of threads to be used
       ULONG   Options;  // combination of DOKAN_OPTIONS_*
       ULONG64 GlobalContext;  // FileSystem can use this variable
       LPCWSTR MountPoint;  // mount point "M:\" (drive letter) or
                            // "C:\mount\dokan" (path in NTFS)
   } DOKAN_OPTIONS, *PDOKAN_OPTIONS;

   int DOKANAPI DokanMain(
       PDOKAN_OPTIONS    DokanOptions,
       PDOKAN_OPERATIONS DokanOperations);

As stated above, the file system can be mounted by invoking DokanMain
function. The function blocks until the file system is unmounted. File
system applications should fill DokanOptions with options for Dokan
runtime library and DokanOperations with function pointers for file
system operations (such as CreateFile, ReadFile, CloseHandle, ...)
before passing these parameters to DokanMain function.  Functions in
DokanOperations structure need to be thread-safe, because they are
called in several threads (not the thread invoked DokanMain) with
different execution contexts.

Dokan options are as follows:

  Version :
    The version of Dokan library. You have to set a supported version.
    Dokan library may change the behavior based on this version number.
    ie. 530 (Dokan 0.5.3)
  ThreadCount :
    The number of threads internaly used by the Dokan library.
    If this value is set to 0, the default value will be used.
    When debugging the file system, file system application should
    set this value to 1 to avoid nondeterministic behaviors of
    multithreading.
  Options :
    A Combination of DOKAN_OPTION_* constants.
  GlobalContext :
    Your filrsystem can use this variable to store a mount specific
    structure.
  MountPoint :
    A mount point. "M:\" drive letter or "C:\mount\dokan" a directory
    (needs empty) in NTFS

If the mount operation succeeded, the return value is DOKAN_SUCCESS,
otherwise, the following error code is returned.

   #define DOKAN_SUCCESS                0
   #define DOKAN_ERROR                 -1 /* General Error */
   #define DOKAN_DRIVE_LETTER_ERROR    -2 /* Bad Drive letter */
   #define DOKAN_DRIVER_INSTALL_ERROR  -3 /* Can't install driver */
   #define DOKAN_START_ERROR           -4 /* Driver something wrong */
   #define DOKAN_MOUNT_ERROR           -5 /* Can't assign a drive letter or mount point */
   #define DOKAN_MOUNT_POINT_ERROR     -6 /* Mount point is invalid */


Unmounting
==========

File system can be unmounted by calling the function DokanUnmount.  In
most cases when the programs or shells use the file system hang,
unmount operation will solve the problems by bringing the system to
the previous state when the file system is not mounted.

User may use DokanCtl to unmount file system like this:
   > dokanctl.exe /u DriveLetter


Misc
====

If there are bugs in Dokan library or file system applications which
use the library, you will get the Windows blue screen. Therefore, it
is strongly recommended to use Virtual Machine when you develop file
system applications.

