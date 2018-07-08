#include "stdafx.h"

#include <string>


static HRESULT StartDokanCtlProcess(LPWSTR buffer, LPWSTR param) {
  STARTUPINFO si;
  PROCESS_INFORMATION pi;
  HRESULT hr = S_OK;

  WcaLog(LOGMSG_STANDARD, "StartDokanCtlProcess with param \"%S\".", param);

  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  /* INSTALLFOLDER=C:\Program Files\Dokan\DokanLibrary-1.1.0\ */
  std::wstring installFolderPath(buffer);
  size_t installerFolderLength = strlen("INSTALLFOLDER=");
  if (installFolderPath.length() <= installerFolderLength) {
    WcaLog(LOGMSG_STANDARD, "Could not retrieve INSTALLFOLDER value from CustomActionData");
    return E_ABORT;
  }

  installFolderPath.erase(0, strlen("INSTALLFOLDER="));
  WcaLog(LOGMSG_STANDARD, "InstallFolderPath=\"%S\"", installFolderPath.c_str());
  std::wstring installDokanctlPathString =
      installFolderPath + L"dokanctl.exe " + std::wstring(param);
  LPWSTR installDokanctlPath = &installDokanctlPathString[0];

  WcaLog(LOGMSG_STANDARD, "InstallDokanctlPath=\"%S\"", installDokanctlPath);

  if (!CreateProcess(NULL, installDokanctlPath, NULL, NULL, FALSE, 0, NULL,
                     NULL, &si,
                     &pi)) {
    WcaLog(LOGMSG_STANDARD, "CreateProcess failed");
    return E_ABORT;
  }

  // Wait until child processes exit.
  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit_code;
  if (!GetExitCodeProcess(pi.hProcess, &exit_code))
    ExitOnLastError(hr, "GetExitCodeProcess failed");

  if (exit_code != 0)
    ExitOnFailure(hr, "dokanctl return an error: \"%ld\"", exit_code);

LExit:
  // Close process and thread handles.
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

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

UINT __stdcall ExecuteInstall(
	MSIHANDLE hInstall
	)
{
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

LExit:
  if (buffer)
    delete buffer;

  er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
  return WcaFinalize(er);
}

// DllMain - Initialize and cleanup WiX custom action utils.
extern "C" BOOL WINAPI DllMain(
	__in HINSTANCE hInst,
	__in ULONG ulReason,
	__in LPVOID
	)
{
	switch(ulReason)
	{
	case DLL_PROCESS_ATTACH:
		WcaGlobalInitialize(hInst);
		break;

	case DLL_PROCESS_DETACH:
		WcaGlobalFinalize();
		break;
	}

	return TRUE;
}
