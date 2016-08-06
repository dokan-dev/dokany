/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2015 - 2016 Adrien J. <liryna.stark@gmail.com> and Maxime C. <maxime@islog.com>
  Copyright (C) 2007 - 2011 Hiroki Asakawa <info@dokan-dev.net>

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

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

#include "../dokan/dokan.h"
#include "../dokan/dokanc.h"
#include <Shlobj.h>

#define DOKAN_DRIVER_FULL_PATH                                                 \
  L"%SystemRoot%\\system32\\drivers\\dokan" DOKAN_MAJOR_API_VERSION L".sys"

int ShowUsage() {
  fprintf(stderr,
          "dokanctl /u MountPoint\n"
          "dokanctl /u M\n"
          "dokanctl /i [d||n]\n"
          "dokanctl /r [d||n]\n"
          "dokanctl /v\n"
          "\n"
          "Example:\n"
          "  /u M                : Unmount M: drive\n"
          "  /u C:\\mount\\dokan : Unmount mount point C:\\mount\\dokan\n"
          "  /u 1                : Unmount mount point 1\n"
          "  /i d                : Install driver\n"
          "  /i n                : Install network provider\n"
          "  /r d                : Remove driver\n"
          "  /r n                : Remove network provider\n"
          "  /l a                : List current mount points\n"
          "  /d [0-9]            : Enable Kernel Debug output\n"
          "  /v                  : Print Dokan version\n");
  return EXIT_FAILURE;
}

int Unmount(LPCWSTR MountPoint) {
  int status = EXIT_SUCCESS;

  if (!DokanRemoveMountPoint(MountPoint)) {
    status = EXIT_FAILURE;
  }

  fwprintf(stdout, L"Unmount status = %d\n", status);
  return status;
}

int InstallDriver(LPCWSTR driverFullPath) {
  fprintf(stdout, "Installing driver...\n");
  if (GetFileAttributes(driverFullPath) == INVALID_FILE_ATTRIBUTES) {
    fwprintf(stderr, L"Error the file '%s' does not exist.\n", driverFullPath);
    return EXIT_FAILURE;
  }

  if (!DokanServiceInstall(DOKAN_DRIVER_SERVICE, SERVICE_FILE_SYSTEM_DRIVER,
                           DOKAN_DRIVER_FULL_PATH)) {
    fprintf(stderr, "Driver install failed\n");
    return EXIT_FAILURE;
  }

  fprintf(stdout, "Driver installation succeeded!\n");
  return EXIT_SUCCESS;
}

int DeleteDokanService(LPCWSTR ServiceName) {
  fwprintf(stdout, L"Removing '%s'...\n", ServiceName);
  if (!DokanServiceDelete(ServiceName)) {
    fwprintf(stderr, L"Error removing '%s'\n", ServiceName);
    return EXIT_FAILURE;
  }
  fwprintf(stdout, L"'%s' removed.\n", ServiceName);
  return EXIT_SUCCESS;
}

#define GetOption(argc, argv, index)                                           \
  (((argc) > (index) && wcslen((argv)[(index)]) == 2 &&                        \
    (argv)[(index)][0] == L'/')                                                \
       ? towlower((argv)[(index)][1])                                          \
       : L'\0')

int __cdecl wmain(int argc, PWCHAR argv[]) {
  size_t i;
  WCHAR fileName[MAX_PATH];
  WCHAR driverFullPath[MAX_PATH] = {0};
  PVOID wow64OldValue;
  BOOL isAdmin;

  isAdmin = IsUserAnAdmin();

  DokanUseStdErr(TRUE); // Set dokan library debug output

  Wow64DisableWow64FsRedirection(&wow64OldValue); // Disable system32 direct
  // setlocale(LC_ALL, "");

  GetModuleFileName(NULL, fileName, MAX_PATH);

  // search the last "\"
  for (i = wcslen(fileName) - 1; i > 0 && fileName[i] != L'\\'; --i) {
    ;
  }
  fileName[i] = L'\0';

  ExpandEnvironmentStringsW(DOKAN_DRIVER_FULL_PATH, driverFullPath, MAX_PATH);

  fwprintf(stdout, L"Driver path: '%s'\n", driverFullPath);

  WCHAR option = GetOption(argc, argv, 1);
  if (option == L'\0') {
    return ShowUsage();
  }

  if (!isAdmin &&
      (option == L'i' || option == L'r' || option == L'd' || option == L'u')) {
    fprintf(stderr, "Admin rights required to process this operation\n");
    return EXIT_FAILURE;
  }

  switch (option) {
  // Admin rights required
  case L'i': {
    WCHAR type = towlower(argv[2][0]);
    if (type == L'd') {
      return InstallDriver(driverFullPath);
    } else if (type == L'n') {
      if (DokanNetworkProviderInstall())
        fprintf(stdout, "network provider install ok\n");
      else
        fprintf(stderr, "network provider install failed\n");
    } else {
      goto DEFAULT;
    }
  } break;

  case L'r': {
    WCHAR type = towlower(argv[2][0]);
    if (type == L'd') {
      return DeleteDokanService(DOKAN_DRIVER_SERVICE);
    } else if (type == L'n') {
      if (DokanNetworkProviderUninstall())
        fprintf(stdout, "network provider remove ok\n");
      else
        fprintf(stderr, "network provider remove failed\n");
    } else {
      goto DEFAULT;
    }
  } break;

  case L'd': {
    WCHAR type = towlower(argv[2][0]);
    if (L'0' > type || type > L'9')
      goto DEFAULT;

    ULONG mode = type - L'0';
    if (DokanSetDebugMode(mode)) {
      fprintf(stdout, "set debug mode ok\n");
    } else {
      fprintf(stderr, "set debug mode failed\n");
    }
  } break;

  case L'u': {
    if (argc < 3) {
      goto DEFAULT;
    }
    return Unmount(argv[2]);
  } break;

  // No admin rights required
  case L'l': {
    ULONG nbRead = 0;
    DOKAN_CONTROL dokanControl[DOKAN_MAX_INSTANCES];
    if (DokanGetMountPointList(dokanControl, DOKAN_MAX_INSTANCES, FALSE,
                               &nbRead)) {
      fwprintf(stdout, L"  Mount points: %d\n", nbRead);
      for (unsigned int p = 0; p < nbRead; ++p) {
        fwprintf(stdout, L"  %d# MountPoint: %s - UNC: %s - DeviceName: %s\n",
                 p, dokanControl[p].MountPoint, dokanControl[p].UNCName,
                 dokanControl[p].DeviceName);
      }
    } else {
      fwprintf(stderr, L"  Cannot retrieve mount point list.\n");
    }
  } break;

  case L'v': {
    fprintf(stdout, "dokanctl : %s %s\n", __DATE__, __TIME__);
    fprintf(stdout, "Dokan version : %d\n", DokanVersion());
    fprintf(stdout, "Dokan driver version : 0x%lx\n", DokanDriverVersion());
  } break;

  DEFAULT:
  default:
    fprintf(stderr, "Unknown option - Use /? to show usage\n");
  }

  return EXIT_SUCCESS;
}
