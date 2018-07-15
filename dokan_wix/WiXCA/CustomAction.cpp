#include "stdafx.h"

#include <string>


static HRESULT StartDokanCtlProcess(LPWSTR buffer, LPWSTR param) {
  STARTUPINFO si;
  PROCESS_INFORMATION pi;
  HRESULT hr = S_OK;
  SECURITY_ATTRIBUTES saAttr;
  HANDLE childStdOutRead = NULL;
  HANDLE childStdOutWrite = NULL;

  WcaLog(LOGMSG_STANDARD, "StartDokanCtlProcess with param \"%S\".", param);

  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  /* INSTALLFOLDER=C:\Program Files\Dokan\DokanLibrary-1.1.0\ */
  std::wstring installFolderPath(buffer);
  size_t installerFolderLength = strlen("INSTALLFOLDER=");
  if (installFolderPath.length() <= installerFolderLength) {
    WcaLog(LOGMSG_STANDARD,
           "Could not retrieve INSTALLFOLDER value from CustomActionData");
    return E_ABORT;
  }

  installFolderPath.erase(0, strlen("INSTALLFOLDER="));
  WcaLog(LOGMSG_STANDARD, "InstallFolderPath=\"%S\"",
         installFolderPath.c_str());
  std::wstring installDokanctlPathString =
      installFolderPath + L"dokanctl.exe " + std::wstring(param);
  LPWSTR installDokanctlPath = &installDokanctlPathString[0];

  WcaLog(LOGMSG_STANDARD, "InstallDokanctlPath=\"%S\"", installDokanctlPath);

  // Set the bInheritHandle flag so pipe handles are inherited.
  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle = TRUE;
  saAttr.lpSecurityDescriptor = NULL;

  WcaLog(LOGMSG_STANDARD, "Create redirect pipe.");
  // Create a pipe for the child process's STDOUT.
  if (!CreatePipe(&childStdOutRead, &childStdOutWrite, &saAttr, 0)) {
    WcaLog(LOGMSG_STANDARD, "StdoutRd CreatePipe");
    return E_ABORT;
  }
  // Ensure the read handle to the pipe for STDOUT is not inherited.
  if (!SetHandleInformation(childStdOutRead, HANDLE_FLAG_INHERIT, 0)) {
    CloseHandle(childStdOutWrite);
    CloseHandle(childStdOutRead);
    WcaLog(LOGMSG_STANDARD, "Stdout SetHandleInformation");
    return E_ABORT;
  }

  si.hStdError = childStdOutWrite;
  si.hStdOutput = childStdOutWrite;
  si.dwFlags |= STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
  si.wShowWindow = SW_HIDE;

  WcaLog(LOGMSG_STANDARD, "CreateProcess dokanctl.");
  if (!CreateProcess(NULL, installDokanctlPath, NULL, NULL, TRUE, 0, NULL,
                     NULL, &si, &pi)) {
    WcaLog(LOGMSG_STANDARD, "CreateProcess failed");
    return E_ABORT;
  }

  WcaLog(LOGMSG_STANDARD, "Wait dokanctl finish...");
  // Wait until child processes exit.
  WaitForSingleObject(pi.hProcess, INFINITE);

  CloseHandle(childStdOutWrite);
  childStdOutWrite = NULL;

  WcaLog(LOGMSG_STANDARD, "GetExitCodeProcess dokanctl.");
  DWORD exit_code;
  if (!GetExitCodeProcess(pi.hProcess, &exit_code))
    ExitOnLastError(hr, "GetExitCodeProcess failed");

  CHAR outbuf[4096];
  DWORD bytes_read;
  CHAR tBuf[257];

  WcaLog(LOGMSG_STANDARD, "Read dokanctl output");
  strcpy_s(outbuf, sizeof(outbuf), "");
  for (;;) {
    if (!ReadFile(childStdOutRead, tBuf, 256, &bytes_read, NULL)) {
      WcaLog(LOGMSG_STANDARD, "ReadFile error: %u", GetLastError());
      break;
    }
    WcaLog(LOGMSG_STANDARD,"ReadFile, read %u bytes", bytes_read);
    if (bytes_read > 0) {
      tBuf[bytes_read] = '\0';
      strcat_s(outbuf, sizeof(outbuf), tBuf);
    }
  }
  WcaLog(LOGMSG_STANDARD, "dokanctl output : \"%s\"", outbuf);

  if (exit_code != 0) {
    WcaLog(LOGMSG_STANDARD, "dokanctl return an error (%ld)", exit_code);
    hr = E_FAIL;
  }

LExit:
  WcaLog(LOGMSG_STANDARD, "Cleanup dokanctl handle.");
  // Close process and thread handles.
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  // Close pipes handles.
  if (childStdOutRead)
    CloseHandle(childStdOutRead);
  if (childStdOutWrite)
    CloseHandle(childStdOutWrite);

  WcaLog(LOGMSG_STANDARD, "StartDokanCtlProcess exit.");

  return hr;
}

static WCHAR *GetCustomActionData(MSIHANDLE hInstall) {
  HRESULT hr = S_OK;
  TCHAR *buffer = NULL;
  DWORD bufferLen = 0;

  WcaLog(LOGMSG_STANDARD, "GetCustomActionData start.");

  UINT customDataReturn =
      MsiGetProperty(hInstall, L"CustomActionData", L"", &bufferLen);
  if (ERROR_MORE_DATA != customDataReturn)
    ExitOnFailure(hr, "Could not get MsiGetProperty CustomActionData");

  ++bufferLen;
  buffer = new WCHAR[bufferLen];
  ExitOnNullWithLastError(buffer, hr,
                          "Could not allocate memory for MsiGetProperty");

  customDataReturn =
      MsiGetProperty(hInstall, L"CustomActionData", buffer, &bufferLen);

  WcaLog(LOGMSG_STANDARD, "CustomActionData=\"%S\"", buffer);

LExit:
  WcaLog(LOGMSG_STANDARD, "GetCustomActionData exit.");

  return buffer;
}

UINT __stdcall ExecuteInstall(MSIHANDLE hInstall) {
  HRESULT hr = S_OK;
  UINT er = ERROR_SUCCESS;

  hr = WcaInitialize(hInstall, "ExecuteInstall");
  ExitOnFailure(hr, "Failed to initialize");

  WcaLog(LOGMSG_STANDARD, "ExecuteInstall Initialized.");

  WCHAR *buffer = GetCustomActionData(hInstall);
  ExitOnNullWithLastError(buffer, hr, "GetCustomActionData failed");

  //hr = StartDokanCtlProcess(buffer, L"/i a");
  //ExitOnFailure(hr, "StartDokanCtlProcess failed");
  hr = StartDokanCtlProcess(buffer, L"/i n");
  ExitOnFailure(hr, "StartDokanCtlProcess failed");

  WcaLog(LOGMSG_STANDARD, "ExecuteInstall done.");

LExit:
  if (buffer)
    delete buffer;

  er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
  return WcaFinalize(er);
}

UINT __stdcall ExecuteUninstall(MSIHANDLE hInstall) {
  HRESULT hr = S_OK;
  UINT er = ERROR_SUCCESS;

  hr = WcaInitialize(hInstall, "ExecuteUninstall");
  ExitOnFailure(hr, "Failed to initialize");

  WcaLog(LOGMSG_STANDARD, "ExecuteUninstall Initialized.");

  WCHAR *buffer = GetCustomActionData(hInstall);
  ExitOnNullWithLastError(buffer, hr, "GetCustomActionData failed");

  // hr = StartDokanCtlProcess(buffer, L"/r a");
  // ExitOnFailure(hr, "failed to StartDokanCtlProcess");
  hr = StartDokanCtlProcess(buffer, L"/r n");
  ExitOnFailure(hr, "StartDokanCtlProcess failed");

  WcaLog(LOGMSG_STANDARD, "ExecuteUninstall done.");

LExit:
  if (buffer)
    delete buffer;

  er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
  return WcaFinalize(er);
}

// DllMain - Initialize and cleanup WiX custom action utils.
extern "C" BOOL WINAPI DllMain(__in HINSTANCE hInst, __in ULONG ulReason,
                               __in LPVOID) {
  switch (ulReason) {
  case DLL_PROCESS_ATTACH:
    WcaGlobalInitialize(hInst);
    break;

  case DLL_PROCESS_DETACH:
    WcaGlobalFinalize();
    break;
  }

  return TRUE;
}
