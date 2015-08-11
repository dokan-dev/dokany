# Dokany
[![Build status](https://ci.appveyor.com/api/projects/status/4tpt4v8btyahh3le/branch/master?svg=true)](https://ci.appveyor.com/project/Maxhy/dokany/branch/master)

## What is Dokan
When you want to create a new file system on Windows, other than FAT or NTFS, you need to develop a file system
driver. Developing a device driver that works in kernel mode on
windows is extremely technical. By using Dokan, you can create
your own file systems very easily without writing device drivers. Dokan
is similar to FUSE (Linux file system in user space) but works on Windows.

## What is Dokany
*Dokany is a fork of Dokan 0.6.0 with bug fixes, clean change history and updated to build with latest tools.*

Because the original [Dokan](http://dokan-dev.net/en/docs/) project is no longer maintained.

Because other forks like [DokanX](https://github.com/BenjaminKim/dokanx) have undesired changes or are not really maintained.


**Signed x86 / x64 drivers** are provided at each release.

## Licensing
Dokan contains LGPL and MIT licensed programs.

- user-mode library (dokan.dll)  LGPL
- driver (dokan.sys)             LGPL
- network library (dokannp.dll)  LGPL
- fuse library (dokanfuse.lib)   LGPL
- control program (dokanctl.exe) MIT
- mount service (mounter.exe)    MIT
- samples (mirror.c)             MIT

For details, please check license files.
 * **LGPL** license.lgpl.txt
 * **GPL**  license.gpl.txt
 * **MIT**  license.mit.txt

You can obtain source files from https://dokan-dev.github.io

## Environment
Dokan works on
 * Windows 8.1
 * Windows Server 2012 R2
 * Windows 8
 * Windows Server 2012
 * Windows 7

## How it works
Dokan library contains a user mode DLL (dokan.dll) and a kernel mode
file system driver (dokan.sys). Once Dokan file system driver is
installed, you can create file systems which can be seen as normal
file systems in Windows. The application that creates file systems
using Dokan library is called File system application.

File operation requests from user programs (e.g., CreateFile, ReadFile,
WriteFile, ...) will be sent to the Windows I/O subsystem (runs in kernel
mode) which will subsequently forward the requests to the Dokan file system
driver (dokan.sys). By using functions provided by the Dokan user mode
library (dokan.dll), file system applications are able to register
callback functions to the file system driver. The file system driver
will invoke these callback routines in order to response to the
requests it received. The results of the callback routines will be
sent back to the user program.

For example, when Windows Explorer requests to open a directory, the
OpenDirectory request will be sent to Dokan file system driver and the
driver will invoke the OpenDirectory callback provided by the file system
application. The results of this routine are sent back to Windows Explorer
as the response to the OpenDirectory request. Therefore, the Dokan file
system driver acts as a proxy between user programs and file system
applications. The advantage of this approach is that it allows
programmers to develop file systems in user mode which is safe and
easy to debug.
 
To learn more about Dokan file system development, see the [API documentation](https://github.com/dokan-dev/dokany/wiki/API).

## Build
In short, download and install the [Visual Studio 2013 with WDK 8.1 Update](https://msdn.microsoft.com/en-us/windows/hardware/gg454513.aspx)

For details, see the [build page](https://github.com/dokan-dev/dokany/wiki/Build).

## Installation
For manual installation, see the [installation page](https://github.com/dokan-dev/dokany/wiki/Installation).

## Contribute
You want Dokan to get better? Contribute!


Learn the code and suggest your changes on [GitHub repository](https://github.com/dokan-dev).

Detect defects and report them on [GitHub issue tracker](https://github.com/dokan-dev/dokany/issues).

Ask and answer questions on [Google discussion group](https://groups.google.com/forum/#!forum/dokan).
