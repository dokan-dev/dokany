Dokan Doxygen {#mainpage}
==================================================

This is the homepage of the Doxygen generated documentation for
Dokany.

We recommend you take a look at [our wiki](https://github.com/dokan-dev/dokany/wiki) for
an introduction of how to use the library. You may find mirror samples using Dokan API or Fuse API [here](https://github.com/dokan-dev/dokany/tree/master/samples) useful.


## How to create your file systems

### Dokan operations

To create a file system, an application needs to implement functions in
DOKAN_OPERATIONS structure (declared in dokan.h). Once implemented,
you can invoke \ref DokanMain function with DOKAN_OPERATIONS as parameter
in order to mount the file system. The semantics of functions in
DOKAN_OPERATIONS are similar to corresponding Windows APIs with the same
name. The parameters for these functions are therefore the same as for
their counterpart Windows APIs. These functions are called from many
threads so they need to be thread-safe, otherwise many problems may
occur.

These functions are typically invoked in this sequence:

1. \ref DOKAN_OPERATIONS.ZwCreateFile
2. Other functions (like DOKAN_OPERATIONS.ReadFile or DOKAN_OPERATIONS.WriteFile)
3. \ref DOKAN_OPERATIONS.Cleanup
4. \ref DOKAN_OPERATIONS.CloseFile

Before file access operations (listing directory entries, reading file
attributes, ...), DOKAN_OPERATIONS.ZwCreateFile is always invoked. On other hand, the function DOKAN_OPERATIONS.Cleanup always
gets called by the Dokan file system driver when the file is closed by
the \c CloseHandle Windows API.

Each function should return \c STATUS_SUCCESS when the operation succeeds, otherwise if an error occurs, return the proper \c NTSTATUS (https://support.microsoft.com/en-us/kb/113996).
For example, when ZwCreateFile can't open a file (Win32 error \c ERROR_FILE_NOT_FOUND), you should return \c STATUS_OBJECT_NAME_NOT_FOUND.
Dokan has an exported helper that can be used to convert Win32 Error to \c NTSTATUS with \ref DokanNtStatusFromWin32.

There is a case with DOKAN_OPERATIONS.ZwCreateFile that should return \c STATUS_OBJECT_NAME_COLLISION instead of \c STATUS_SUCCESS when the
param CreationDisposition is \c CREATE_ALWAYS or \c OPEN_ALWAYS and the file requested successfully opened already existed. 

Note: when applications make use of memory mapped
files, WriteFile or ReadFile functions may be invoked after Cleanup in
order to complete the I/O operations. The file system application
should also properly work in this case.

### Dokan File Info Life time

The last parameter of each DOKAN_OPERATIONS functions is a DOKAN_FILE_INFO struct.
Each file handle from user mode is associated with a DOKAN_FILE_INFO
struct. Hence, the content of the struct does not change if the same
file handle is used. The struct is created when the file is opened by
\c CreateFile system call and destroyed when the file is closed by
\c CloseHandle system call. It store all file information on the current operation.

DOKAN_FILE_INFO.Context is an arbitrary value assigned by the file system
application. File system application can freely use this variable to
store values that are constant in a file access session (the period
from \c CreateFile to \c CloseFile) such as file handle, etc.

When the DOKAN_FILE_INFO.IsDirectory is set to \c TRUE, the file under the
operation is a directory. When it is \c FALSE, the file system
application programmers are required to set the variable to \c TRUE if
the current operation acts on a directory. If the value is \c FALSE and
the current operation is not acting on a directory, the programmers
should not change the variable. Note that setting the variable to \c TRUE
when a directory is accessed is very important for the Dokan
library. If it is not set correctly, the library does not know the
operation is acting on a directory and many problems may occur.

DOKAN_OPERATIONS.Cleanup is invoked when the function CloseHandle in Windows API is
executed. If the file system application stored a file handle in the
Context variable when the function DOKAN_OPERATIONS.ZwCreateFile is invoked, this should
be closed in the \c Cleanup function, not in \c CloseFile function. If the
user application calls CloseHandle and subsequently opens the same
file, the \c CloseFile function of the file system application may not be
invoked before the \c CreateFile API is called and therefore may cause a sharing
violation error since the \c HANDLE has not been closed. 

### Find Files

DOKAN_OPERATIONS.FindFiles or DOKAN_OPERATIONS.FindFilesWithPattern are called in order to respond to
directory listing requests.You should implement either one of the two functions. 

For each directory entry, file system applications should call the function FillFindData (passed as a
function pointer to FindFiles, FindFilesWithPattern) with the
\c WIN32_FIND_DATAW structure filled with directory information:
\c FillFindData( &win32FindDataw, DokanFileInfo ).  It is the
responsibility of file systems to process wildcard patterns because
shells in Windows are not designed to work properly with pattern
matching. When file system application only implement FindFiles, the
wildcard patterns are automatically processed by the Dokan
library. You can control wildcard matching by implementing
DOKAN_OPERATIONS.FindFilesWithPattern function.  The function \ref DokanIsNameInExpression
exported can be used to process wildcard matching.

### Mounting

As stated above, the file system can be mounted by invoking \ref DokanMain
function. The function blocks until the file system is unmounted. File
system applications should fill DOKAN_OPTIONS struct to describe the futur device and DOKAN_OPERATIONS with function pointers for file
system operations (such as ZwCreateFile, ReadFile, CloseFile, ...)
before passing these parameters to DokanMain function.  Functions in
DOKAN_OPERATIONS struct need to be thread-safe, because they are
called in several threads (not the thread invoking DokanMain) with
different execution contexts.
\ref DokanMain can instantly return a \ref DokanMainResult status in case of mount failure.

DOKAN_OPTIONS.Options with \ref DOKAN_OPTION flags describe the behavior of the device.

### Unmounting

File system can be unmounted by calling the function \ref DokanUnmount or
\ref DokanRemoveMountPoint.  In most cases when the programs or shells using
the file system hang, unmount operation will solve the problem by
bringing the system to a previous state when the file system was not
mounted.

User may use command line to unmount file system like this:
   > dokanctl.exe /u DriveLetter

## Network Provider

If mounting is done with \ref DOKAN_OPTION_NETWORK, you need Dokan Network Provider for it to work correctly.
This file (dokannp1.dll) *must* be copied to `%WINDIR%\system32` and you can register the provider on your system with `dokanctl.exe /i n` command.

Without this [Network Provider](https://msdn.microsoft.com/en-us/library/windows/desktop/aa378776%28v=vs.85%29.aspx), Windows Explorer will not handle well Virtual drives mounted as network share and your drive could appears disconnected.

If Network Redirector is setup by setting up an UNC Name, Dokan Network Provider will assign UNC Name to the drive label automatically.

## Testing your FileSystem

You can test you filesystem by using different tools.
Microsoft has his own tools like [Runkarr](https://msdn.microsoft.com/en-us/library/windows/hardware/hh998457%28v=vs.85%29.aspx), [ IFSTest](https://msdn.microsoft.com/en-us/library/gg607473%28v=vs.85%29.aspx) or [Device Fundamentals](https://msdn.microsoft.com/windows/hardware/drivers/develop/how-to-select-and-configure-the-device-fundamental-tests).

There is also [WinFSTest](https://github.com/Liryna/winfstest), Dokany is automatically running it on the sample mirror at every commit with appveyor.

For C# developers, there is [DokanNet.Tests](https://github.com/dokan-dev/dokan-dotnet/tree/master/DokanNet.Tests) made by viciousviper that currently only test C# Mirror sample but can easily be changed to test a C# Dokan FS.

## Misc

If there are bugs in Dokan library or file system applications which
use the library, you will get the Windows blue screen. Therefore, it
is strongly recommended to use Virtual Machine when you develop file
system applications.