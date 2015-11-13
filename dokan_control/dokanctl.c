/*

Copyright (c) 2007, 2008 Hiroki Asakawa asakaw@gmail.com

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

#include <windows.h>
#include <Shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>

#include "../dokan/dokan.h"
#include "../dokan/dokanc.h"

#define DOKAN_DRIVER_FULL_PATH L"%SystemRoot%\\system32\\drivers\\dokan.sys"

int ShowMountList() {
  DOKAN_CONTROL control;
  ZeroMemory(&control, sizeof(DOKAN_CONTROL));

  control.Type = DOKAN_CONTROL_LIST;
  control.Option = 0;
  control.Status = DOKAN_CONTROL_SUCCESS;

  while (DokanMountControl(&control)) {
    if (control.Status == DOKAN_CONTROL_SUCCESS) {
      fwprintf(stderr, L"[% 2d] MountPoint: %s\n     DeviceName: %s\n",
               control.Option, control.MountPoint, control.DeviceName);
      control.Option++;
    } else {
      return EXIT_SUCCESS;
    }
  }
  return EXIT_SUCCESS;
}

int ShowUsage() {
  fprintf(stderr,
          "dokanctl /u MountPoint (/f)\n"
          "dokanctl /m\n"
          "dokanctl /i [d|s|a|n]\n"
          "dokanctl /r [d|s|a|n]\n"
          "dokanctl /v\n"
          "\n"
          "Example:\n"
          "  /u M                : Unmount M: drive\n"
          "  /u C:\\mount\\dokan : Unmount mount point C:\\mount\\dokan\n"
          "  /u 1                : Unmount mount point 1\n"
          "  /u M /f             : Force unmount M: drive\n"
          "  /m                  : Print mount points list\n"
          "  /i d                : Install driver\n"
          "  /i s                : Install mounter service\n"
          "  /i a                : Install driver and mounter service\n"
          "  /i n                : Install network provider\n"
          "  /r d                : Remove driver\n"
          "  /r s                : Remove mounter service\n"
          "  /r a                : Remove driver and mounter service\n"
          "  /r n                : Remove network provider\n"
          "  /d [0-9]            : Enable Kernel Debug output\n"
          "  /v                  : Print Dokan version\n");
  return EXIT_FAILURE;
}

int Unmount(LPCWSTR MountPoint, BOOL ForceUnmount) {
  int status = EXIT_SUCCESS;
  DOKAN_CONTROL control;
  ZeroMemory(&control, sizeof(DOKAN_CONTROL));

  if (wcslen(MountPoint) == 1 && L'0' <= MountPoint[0] &&
      MountPoint[0] <= L'9') {
    control.Type = DOKAN_CONTROL_LIST;
    control.Option = MountPoint[0] - L'0';
    DokanMountControl(&control);

    if (control.Status != DOKAN_CONTROL_SUCCESS ||
        (control.Status == DOKAN_CONTROL_SUCCESS &&
         !DokanRemoveMountPoint(control.MountPoint))) {
      fwprintf(stderr, L"Mount entry %d not found\n", control.Option);
      status = EXIT_FAILURE;
    }
  } else if (ForceUnmount) {
    control.Type = DOKAN_CONTROL_UNMOUNT;
    control.Option = DOKAN_CONTROL_OPTION_FORCE_UNMOUNT;
    wcscpy_s(control.MountPoint, sizeof(control.MountPoint) / sizeof(WCHAR),
             MountPoint);
    DokanMountControl(&control);

    if (control.Status != DOKAN_CONTROL_SUCCESS)
      status = EXIT_FAILURE;

    fwprintf(stderr, L"Unmount status %d - %s\tn", status, MountPoint);

  } else if (!DokanRemoveMountPoint(MountPoint))
    status = EXIT_FAILURE;

  fwprintf(stderr, L"Unmount status = %d\n", status);
  return status;
}

int InstallDriver(LPCWSTR driverFullPath) {
  fprintf(stderr, "Installing driver...\n");
  if (GetFileAttributes(driverFullPath) == INVALID_FILE_ATTRIBUTES) {
    fwprintf(stderr, L"Error the file '%s' does not exist.\n", driverFullPath);
    return EXIT_FAILURE;
  }

  if (!DokanServiceInstall(DOKAN_DRIVER_SERVICE, SERVICE_FILE_SYSTEM_DRIVER,
                           DOKAN_DRIVER_FULL_PATH)) {
    fprintf(stderr, "Driver install failed\n");
    return EXIT_FAILURE;
  }

  fprintf(stderr, "Driver installation succeeded!\n");
  return EXIT_SUCCESS;
}

int InstallMounter(LPCWSTR mounterFullPath) {
  fprintf(stderr, "Installing mounter...\n");
  if (GetFileAttributes(mounterFullPath) == INVALID_FILE_ATTRIBUTES) {
    fwprintf(stderr, L"Error the file '%s' does not exist.\n", mounterFullPath);
    return EXIT_FAILURE;
  }

  if (!DokanServiceInstall(DOKAN_MOUNTER_SERVICE, SERVICE_WIN32_OWN_PROCESS,
                           mounterFullPath)) {
    fprintf(stderr, "Mounter install failed\n");
    return EXIT_FAILURE;
  }

  fprintf(stderr, "Mounter installation succeeded!\n");
  return EXIT_SUCCESS;
}

int DeleteDokanService(LPCWSTR ServiceName) {
  fwprintf(stderr, L"Removing '%s'...\n", ServiceName);
  if (!DokanServiceDelete(ServiceName)) {
    fwprintf(stderr, L"Error removing '%s'\n", ServiceName);
    return EXIT_FAILURE;
  }
  fwprintf(stderr, L"'%s' removed.\n", ServiceName);
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
  WCHAR mounterFullPath[MAX_PATH] = {0};
  WCHAR type;
  PVOID wow64OldValue;

  DokanUseStdErr(TRUE); // Set dokan library debug output

  Wow64DisableWow64FsRedirection(&wow64OldValue); //Disable system32 direct
  // setlocale(LC_ALL, "");

  GetModuleFileName(NULL, fileName, MAX_PATH);

  // search the last "\"
  for (i = wcslen(fileName) - 1; i > 0 && fileName[i] != L'\\'; --i) {
    ;
  }
  fileName[i] = L'\0';

  wcscpy_s(mounterFullPath, MAX_PATH, fileName);
  wcscat_s(mounterFullPath, MAX_PATH, L"\\mounter.exe");
  fwprintf(stderr, L"Mounter path: '%s'\n", mounterFullPath);

  ExpandEnvironmentStringsW(DOKAN_DRIVER_FULL_PATH, driverFullPath, MAX_PATH);

  fwprintf(stderr, L"Driver path: '%s'\n", driverFullPath);

  if (GetOption(argc, argv, 1) == L'v') {
    fprintf(stderr, "dokanctl : %s %s\n", __DATE__, __TIME__);
    fprintf(stderr, "Dokan version : %d\n", DokanVersion());
    fprintf(stderr, "Dokan driver version : 0x%lx\n", DokanDriverVersion());
    return EXIT_SUCCESS;

  } else if (GetOption(argc, argv, 1) == L'm') {
    return ShowMountList();

  } else if (GetOption(argc, argv, 1) == L'u' && argc == 3) {
    return Unmount(argv[2], FALSE);

  } else if (GetOption(argc, argv, 1) == L'u' &&
             GetOption(argc, argv, 3) == L'f' && argc == 4) {
    return Unmount(argv[2], TRUE);

  } else if (argc < 3 || wcslen(argv[1]) != 2 || argv[1][0] != L'/') {
    return ShowUsage();
  }

  type = towlower(argv[2][0]);

  switch (towlower(argv[1][1])) {
  case L'i':
    if (type == L'd') {

      return InstallDriver(driverFullPath);

    } else if (type == L's') {

      return InstallMounter(mounterFullPath);

    } else if (type == L'a') {

      if (InstallDriver(driverFullPath) == EXIT_FAILURE)
        return EXIT_FAILURE;

      if (InstallMounter(mounterFullPath) == EXIT_FAILURE)
        return EXIT_FAILURE;

    } else if (type == L'n') {
      if (DokanNetworkProviderInstall())
        fprintf(stderr, "network provider install ok\n");
      else
        fprintf(stderr, "network provider install failed\n");
    }
    break;

  case L'r':
    if (type == L'd') {

      return DeleteDokanService(DOKAN_DRIVER_SERVICE);

    } else if (type == L's') {

      return DeleteDokanService(DOKAN_MOUNTER_SERVICE);

    } else if (type == L'a') {

      if (DeleteDokanService(DOKAN_MOUNTER_SERVICE) == EXIT_FAILURE)
        return EXIT_FAILURE;

      if (DeleteDokanService(DOKAN_DRIVER_SERVICE) == EXIT_FAILURE)
        return EXIT_FAILURE;

    } else if (type == L'n') {
      if (DokanNetworkProviderUninstall())
        fprintf(stderr, "network provider remove ok\n");
      else
        fprintf(stderr, "network provider remove failed\n");
    }
    break;
  case L'd':
    if (L'0' <= type && type <= L'9') {
      ULONG mode = type - L'0';
      if (DokanSetDebugMode(mode)) {
        fprintf(stderr, "set debug mode ok\n");
      } else {
        fprintf(stderr, "set debug mode failed\n");
      }
    }
    break;
  default:
    fprintf(stderr, "unknown option\n");
  }

  return EXIT_SUCCESS;
}
