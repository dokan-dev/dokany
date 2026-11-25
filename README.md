# Dokany

[![Github All Releases](https://img.shields.io/github/downloads/dokan-dev/dokany/total.svg)](https://github.com/dokan-dev/dokany/releases)
[![Build and Test](https://github.com/dokan-dev/dokany/actions/workflows/build-and-test.yml/badge.svg?branch=master)](https://github.com/dokan-dev/dokany/actions/workflows/build-and-test.yml?query=branch%3Amaster)
[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=dokany&metric=alert_status)](https://sonarcloud.io/dashboard?id=dokany)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/5c2ecf8d8f734437beb795dbe5aaa918)](https://app.codacy.com/gh/dokan-dev/dokany/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)
[![CII Best Practices](https://bestpractices.coreinfrastructure.org/projects/1234/badge)](https://bestpractices.coreinfrastructure.org/projects/1234)
[![FOSSA Status](https://app.fossa.io/api/projects/git%2Bgithub.com%2Fdokan-dev%2Fdokany.svg?type=shield)](https://app.fossa.io/projects/git%2Bgithub.com%2Fdokan-dev%2Fdokany?ref=badge_shield)
[![Average time to resolve an issue](http://isitmaintained.com/badge/resolution/dokan-dev/dokany.svg)](http://isitmaintained.com/project/dokan-dev/dokany "Average time to resolve an issue")
[![Percentage of issues still open](http://isitmaintained.com/badge/open/dokan-dev/dokany.svg)](http://isitmaintained.com/project/dokan-dev/dokany "Percentage of issues still open")

![Dokan Demo](http://dokan-dev.github.io/images/screencast.gif)

## What is Dokan

When you want to create a new file system on Windows, other than FAT or NTFS,
you need to develop a file system driver. Developing a device driver that works
in kernel mode on windows is extremely technical. By using Dokan, you can create
your own file systems very easily without writing device drivers. Dokan is
similar to FUSE (Linux file system in user space) but works on Windows. Moreover,
dokany includes a [FUSE wrapper](https://github.com/dokan-dev/dokany/wiki/FUSE)
that helps you to port your FUSE filesystems without changes.

## What is Dokany

*Dokany is a fork of Dokan 0.6.0 with bug fixes, clean change history and
updated to build with latest tools.*

Because the original Dokan Legacy (< 0.6.0) project is no longer maintained.

Since version 0.8.0, dokany broke compatibility with the dokan API. See
[Choose a version](https://github.com/dokan-dev/dokany/wiki/Installation#choose-a-version)
for more information.

The API has then again changed over time in [1.1.0](https://github.com/dokan-dev/dokany/wiki/Update-Dokan-1.0.0-application-to-Dokany-1.1.0), [2.0.0](https://github.com/dokan-dev/dokany/wiki/Update-Dokan-1.1.0-application-to-Dokany-2.0.0), and [3.0.0](https://github.com/dokan-dev/dokany/wiki/Installation#updating-from-dokany-2x-to-30) (upcoming binary/driver major bump; C API unchanged).

## Benchmark v1.5.1.1000 vs v2.0.3.1000

A benchmark that is testing multiple scenarios repeaditly and sequentially was run 5 times against the `memfs` sample of v1.5.1.1000 and v2.0.3.1000 in an idle environment to precise results.
The detail results can be seen in this spreadsheet [here](https://docs.google.com/spreadsheets/d/1zdJ6fmP_sqUGCM7SLtTle9N3JLyBOEAMRlwDLfUqm4Q/edit?usp=sharing).
As better threading and memory poll were added in v2, it is expected that concurrent scenarios (like those tests) would be even more highly improved.

A sample of the results:
```
Create New      |  +13.55% | List          |  +60.69% | GetAttributes |  +48.78% | Read  | +18-42% |
Open/Overwrite  | +153.41% | ListExactFile | +131.91% | SetAttributes | +120.91% | Write | +10-32% |
RandomOpenClose | +173.05% |               |          | Delete        |  +90.83% |       |         |
```

## Licensing

Dokan contains LGPL and MIT licensed programs.

- user-mode library (dokan3.dll)   **LGPL**
- driver (dokan3.sys)              **LGPL**
- network library (dokannp3.dll)   **LGPL**
- fuse library (dokanfuse3.dll)    **LGPL**
- installer (DokanSetup.exe)       **LGPL**
- control program (dokanctl.exe)   **MIT**
- samples (mirror.exe / memfs.exe) **MIT**

For details, please check the license files.
 * **LGPL** license.lgpl.txt
 * **MIT**  license.mit.txt

You can obtain source files from https://dokan-dev.github.io

## Environment

Dokan works on
 * Windows Server 2025 / 2022 / 2019 / 2016
 * Windows 11 / 10 (64-bit)
 
Platform
 * x64 (including x86 WOW64 user-mode libraries)
 * ARM64

**Signed Release and Debug drivers** are provided at each release for all platforms.

## How it works

Dokan library contains a user mode DLL (dokan3.dll) and a kernel mode file
system driver (dokan3.sys). Once the Dokan file system driver is installed, you can
create file systems which can be seen as normal file systems in Windows. The
application that creates file systems using Dokan library is called File system
application.

File operation requests from user programs (e.g., CreateFile, ReadFile,
WriteFile, ...) will be sent to the Windows I/O subsystem (runs in kernel mode)
which will subsequently forward the requests to the Dokan file system driver
(dokan3.sys). By using functions provided by the Dokan user mode library
(dokan3.dll), file system applications are able to register callback functions
to the file system driver. The file system driver will invoke these callback
routines in order to respond to the requests it received. The results of the
callback routines will be sent back to the user program.

For example, when Windows Explorer requests to open a directory, the CreateFile
with Direction option request will be sent to Dokan file system driver and the
driver will invoke the CreateFile callback provided by the file system
application. The results of this routine are sent back to Windows Explorer as
the response to the CreateFile request. Therefore, the Dokan file system driver
acts as a proxy between user programs and file system applications. The
advantage of this approach is that it allows programmers to develop file systems
in user mode which is safe and easy to debug.
 
To learn more about Dokan file system development, see the
[![API documentation](https://img.shields.io/badge/Documentation-API-green.svg)](https://dokan-dev.github.io/dokany-doc/html/) and the [samples](https://github.com/dokan-dev/dokany/tree/master/samples), especially [dokan_memfs](https://github.com/dokan-dev/dokany/tree/master/samples/dokan_memfs).

## Build

In short, download and install
[Visual Studio 2026](https://visualstudio.microsoft.com/downloads/) with the **Desktop development with C++** workload, Spectre-mitigated libraries, the [Windows SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/), and the **Windows Driver Kit** Visual Studio component, then restore the WDK NuGet packages (`nuget restore sys\packages.config -PackagesDirectory packages`) or install the matching [WDK](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk).

For details, see the
[build page](https://github.com/dokan-dev/dokany/wiki/Build).

## Installation

The latest official and signed build can be downloaded from:
* [Github release page](https://github.com/dokan-dev/dokany/releases)
* `choco install dokany2` (or `dokany3` starting with 3.0.0) or
* `winget install dokan-dev.dokany`

For manual installation, see the
[installation page](https://github.com/dokan-dev/dokany/wiki/Installation).

## Testing

Dokany includes a standalone, MIT-licensed C++20/GoogleTest NTFS conformance
framework in [`tests/fstest/`](tests/fstest/README.md) spanning 12 filesystem
domains that validates Win32 and native NT semantics against real NTFS volumes
and the `memfs` and `mirror` samples across x64 and x86 targets. Tracked Dokan
bugs and unimplemented NTFS features are cataloged with Microsoft specification
references in
[`tests/fstest/CONFORMANCE_GAPS.md`](tests/fstest/CONFORMANCE_GAPS.md).

## Contribute

You want Dokan to get better? Contribute!

Learn the code and suggest your changes on
[GitHub repository](https://github.com/dokan-dev).

Detect defects and report them on
[GitHub issue tracker](https://github.com/dokan-dev/dokany/issues).

Ask and answer questions on
[Github Discussions](https://github.com/dokan-dev/dokany/discussions) or 
[Google discussion group](https://groups.google.com/forum/#!forum/dokan).
