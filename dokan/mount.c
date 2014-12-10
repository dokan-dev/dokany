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


#include <windows.h>
#include <stdio.h>
#include "dokani.h"

static BOOL
DokanServiceCheck(
	LPCWSTR	ServiceName)
{
	SC_HANDLE controlHandle;
	SC_HANDLE serviceHandle;
	SERVICE_STATUS ss;

	controlHandle = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);

	if (controlHandle == NULL) {
		DbgPrint("failed to open SCM: %d\n", GetLastError());
		return FALSE;
	}

	serviceHandle = OpenService(controlHandle, ServiceName,
		SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);

	if (serviceHandle == NULL) {
		CloseServiceHandle(controlHandle);
		return FALSE;
	}
	
	CloseServiceHandle(serviceHandle);
	CloseServiceHandle(controlHandle);

	return TRUE;
}


static BOOL
DokanServiceControl(
	LPCWSTR	ServiceName,
	ULONG	Type)
{
	SC_HANDLE controlHandle;
	SC_HANDLE serviceHandle;
	SERVICE_STATUS ss;
	BOOL result = TRUE;

	controlHandle = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);

	if (controlHandle == NULL) {
		DokanDbgPrint("failed to open SCM: %d\n", GetLastError());
		return FALSE;
	}

	serviceHandle = OpenService(controlHandle, ServiceName,
		SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);

	if (serviceHandle == NULL) {
		DokanDbgPrintW(L"failed to open Service (%s): %d\n", ServiceName, GetLastError());
		CloseServiceHandle(controlHandle);
		return FALSE;
	}
	
	QueryServiceStatus(serviceHandle, &ss);

	if (Type == DOKAN_SERVICE_DELETE) {
		if (DeleteService(serviceHandle)) {
			DokanDbgPrintW(L"Service (%s) deleted\n", ServiceName);
			result = TRUE;
		} else {
			DokanDbgPrintW(L"failed to delete service (%s): %d\n", ServiceName, GetLastError());
			result = FALSE;
		}

	} else if (ss.dwCurrentState == SERVICE_STOPPED && Type == DOKAN_SERVICE_START) {
		if (StartService(serviceHandle, 0, NULL)) {
			DokanDbgPrintW(L"Service (%s) started\n", ServiceName);
			result = TRUE;
		} else {
			DokanDbgPrintW(L"failed to start service (%s): %d\n", ServiceName, GetLastError());
			result = FALSE;
		}
	
	} else if (ss.dwCurrentState == SERVICE_RUNNING && Type == DOKAN_SERVICE_STOP) {

		if (ControlService(serviceHandle, SERVICE_CONTROL_STOP, &ss)) {
			DokanDbgPrintW(L"Service (%s) stopped\n", ServiceName);
			result = TRUE;
		} else {
			DokanDbgPrintW(L"failed to stop service (%s): %d\n", ServiceName, GetLastError());
			result = FALSE;
		}
	}

	CloseServiceHandle(serviceHandle);
	CloseServiceHandle(controlHandle);

	Sleep(100);
	return result;
}



BOOL DOKANAPI
DokanMountControl(PDOKAN_CONTROL Control)
{
	HANDLE pipe;
	DWORD writtenBytes;
	DWORD readBytes;
	DWORD pipeMode;
	DWORD error;

	for (;;) {
		pipe = CreateFile(DOKAN_CONTROL_PIPE,  GENERIC_READ|GENERIC_WRITE,
						0, NULL, OPEN_EXISTING, 0, NULL);
		if (pipe != INVALID_HANDLE_VALUE) {
			break;
		}

		error = GetLastError();
		if (error == ERROR_PIPE_BUSY) {
			if (!WaitNamedPipe(DOKAN_CONTROL_PIPE, NMPWAIT_USE_DEFAULT_WAIT)) {
				DbgPrint("DokanMounter service : ERROR_PIPE_BUSY\n");
				return FALSE;
			}
			continue;
		} else if (error == ERROR_ACCESS_DENIED) {
			DbgPrint("failed to connect DokanMounter service: access denied\n");
			return FALSE;
		} else {
			DbgPrint("failed to connect DokanMounter service: %d\n", GetLastError());
			return FALSE;
		}
	}

	pipeMode = PIPE_READMODE_MESSAGE|PIPE_WAIT;

	if(!SetNamedPipeHandleState(pipe, &pipeMode, NULL, NULL)) {
		DbgPrint("failed to set named pipe state: %d\n", GetLastError());
		CloseHandle(pipe);
		return FALSE;
	}


	if(!TransactNamedPipe(pipe, Control, sizeof(DOKAN_CONTROL),
		Control, sizeof(DOKAN_CONTROL), &readBytes, NULL)) {
		DbgPrint("failed to transact named pipe: %d\n", GetLastError());
	}

	CloseHandle(pipe);
	if(Control->Status != DOKAN_CONTROL_FAIL) {
		return TRUE;
	} else {
		return FALSE;
	}
}



BOOL DOKANAPI
DokanServiceInstall(
	LPCWSTR	ServiceName,
	DWORD	ServiceType,
	LPCWSTR ServiceFullPath)
{
	SC_HANDLE	controlHandle;
	SC_HANDLE	serviceHandle;
	
	controlHandle = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
	if (controlHandle == NULL) {
		DokanDbgPrint("failed to open SCM");
		return FALSE;
	}

	serviceHandle = CreateService(controlHandle, ServiceName, ServiceName, 0,
		ServiceType, SERVICE_AUTO_START, SERVICE_ERROR_IGNORE,
		ServiceFullPath, NULL, NULL, NULL, NULL, NULL);
	
	if (serviceHandle == NULL) {
		if (GetLastError() == ERROR_SERVICE_EXISTS) {
			DokanDbgPrintW(L"Service (%s) is already installed\n", ServiceName);
		} else {
			DokanDbgPrintW(L"failted to install service (%s): %d\n", ServiceName, GetLastError());
		}
		CloseServiceHandle(controlHandle);
		return FALSE;
	}
	
	CloseServiceHandle(serviceHandle);
	CloseServiceHandle(controlHandle);

	DokanDbgPrintW(L"Service (%s) installed\n", ServiceName);

	if (DokanServiceControl(ServiceName, DOKAN_SERVICE_START)) {
		DokanDbgPrintW(L"Service (%s) started\n", ServiceName);
		return TRUE;
	} else {
		DokanDbgPrintW(L"Service (%s) start failed\n", ServiceName);
		return FALSE;
	}
}


BOOL DOKANAPI
DokanServiceDelete(
	LPCWSTR	ServiceName)
{
	if (DokanServiceCheck(ServiceName)) {
		DokanServiceControl(ServiceName, DOKAN_SERVICE_STOP);
		if (DokanServiceControl(ServiceName, DOKAN_SERVICE_DELETE)) {
			return TRUE;
		} else {
			return FALSE;
		}
	}
	return TRUE;
}


BOOL DOKANAPI
DokanUnmount(
	WCHAR	DriveLetter)
{
	WCHAR mountPoint[] = L"M:\\";
	mountPoint[0] = DriveLetter;
	return DokanRemoveMountPoint(mountPoint);
}


BOOL DOKANAPI
DokanRemoveMountPoint(
	LPCWSTR MountPoint)
{
	DOKAN_CONTROL control;
	BOOL result;

	ZeroMemory(&control, sizeof(DOKAN_CONTROL));
	control.Type = DOKAN_CONTROL_UNMOUNT;
	wcscpy_s(control.MountPoint, sizeof(control.MountPoint) / sizeof(WCHAR), MountPoint);

	DbgPrintW(L"DokanRemoveMountPoint %ws\n", MountPoint);

	result = DokanMountControl(&control);
	if (result) {
		DbgPrint("DokanControl recieved DeviceName:%ws\n", control.DeviceName);
		SendReleaseIRP(control.DeviceName);
	} else {
		DbgPrint("DokanRemoveMountPoint failed\n");
	}
	return result;
}


BOOL
DokanMount(
	LPCWSTR	MountPoint,
	LPCWSTR	DeviceName)
{
	DOKAN_CONTROL control;

	ZeroMemory(&control, sizeof(DOKAN_CONTROL));
	control.Type = DOKAN_CONTROL_MOUNT;

	wcscpy_s(control.MountPoint, sizeof(control.MountPoint) / sizeof(WCHAR), MountPoint);
	wcscpy_s(control.DeviceName, sizeof(control.DeviceName) / sizeof(WCHAR), DeviceName);

	return  DokanMountControl(&control);
}


#define DOKAN_NP_SERVICE_KEY	L"System\\CurrentControlSet\\Services\\Dokan"
#define DOKAN_NP_DEVICE_NAME	L"\\Device\\DokanRedirector"
#define DOKAN_NP_NAME			L"DokanNP"
#define DOKAN_NP_PATH			L"System32\\dokannp.dll"
#define DOKAN_NP_ORDER_KEY		L"System\\CurrentControlSet\\Control\\NetworkProvider\\Order"

BOOL DOKANAPI
DokanNetworkProviderInstall()
{
	HKEY key;
	DWORD position;
	DWORD type;
	WCHAR buffer[1024];
	DWORD buffer_size = sizeof(buffer);
	ZeroMemory(&buffer, sizeof(buffer));

	RegCreateKeyEx(HKEY_LOCAL_MACHINE, DOKAN_NP_SERVICE_KEY L"\\NetworkProvider", 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &key, &position);

	RegSetValueEx(key, L"DeviceName", 0, REG_SZ,
		(BYTE*)DOKAN_NP_DEVICE_NAME, (wcslen(DOKAN_NP_DEVICE_NAME)+1) * sizeof(WCHAR));

	RegSetValueEx(key, L"Name", 0, REG_SZ,
		(BYTE*)DOKAN_NP_NAME, (wcslen(DOKAN_NP_NAME)+1) * sizeof(WCHAR));

	RegSetValueEx(key, L"ProviderPath", 0, REG_SZ,
		(BYTE*)DOKAN_NP_PATH, (wcslen(DOKAN_NP_PATH)+1) * sizeof(WCHAR));

    RegCloseKey(key);

	RegOpenKeyEx(HKEY_LOCAL_MACHINE, DOKAN_NP_ORDER_KEY, 0, KEY_ALL_ACCESS, &key);

	RegQueryValueEx(key, L"ProviderOrder", 0, &type, (BYTE*)&buffer, &buffer_size);

	if (wcsstr(buffer, L",Dokan") == NULL) {
		wcscat_s(buffer, sizeof(buffer) / sizeof(WCHAR), L",Dokan");
		RegSetValueEx(key, L"ProviderOrder", 0, REG_SZ,
			(BYTE*)&buffer, (wcslen(buffer) + 1) * sizeof(WCHAR));
	}

    RegCloseKey(key);
	return TRUE;
}


BOOL DOKANAPI
DokanNetworkProviderUninstall()
{
	HKEY key;
	DWORD type;
	WCHAR buffer[1024];
	WCHAR buffer2[1024];

	DWORD buffer_size = sizeof(buffer);
	ZeroMemory(&buffer, sizeof(buffer));
	ZeroMemory(&buffer2, sizeof(buffer));

	RegOpenKeyEx(HKEY_LOCAL_MACHINE, DOKAN_NP_SERVICE_KEY, 0, KEY_ALL_ACCESS, &key);
	RegDeleteKey(key, L"NetworkProvider");

    RegCloseKey(key);

	RegOpenKeyEx(HKEY_LOCAL_MACHINE, DOKAN_NP_ORDER_KEY, 0, KEY_ALL_ACCESS, &key);

	RegQueryValueEx(key, L"ProviderOrder", 0, &type, (BYTE*)&buffer, &buffer_size);

	if (wcsstr(buffer, L",Dokan") != NULL) {
		WCHAR* dokan_pos = wcsstr(buffer, L",Dokan");
		wcsncpy_s(buffer2, sizeof(buffer2) / sizeof(WCHAR), buffer, dokan_pos - buffer);
		wcscat_s(buffer2, sizeof(buffer2) / sizeof(WCHAR), dokan_pos + wcslen(L",Dokan"));
		RegSetValueEx(key, L"ProviderOrder", 0, REG_SZ,
			(BYTE*)&buffer2, (wcslen(buffer2) + 1) * sizeof(WCHAR));
	}

    RegCloseKey(key);

	return TRUE;
}