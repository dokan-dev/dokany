#include <windows.h>
#include <winnetwk.h>
#include <winsvc.h>
#include <stdio.h>
#include <npapi.h>
#include <strsafe.h>

static VOID
DokanDbgPrintW(LPCWSTR format, ...)
{
	WCHAR buffer[512];
	va_list argp;
	va_start(argp, format);
	StringCchVPrintfW(buffer, 127, format, argp);
    va_end(argp);
	OutputDebugStringW(buffer);
}

#define DbgPrintW(format, ...) \
	DokanDbgPrintW(format, __VA_ARGS__)

DWORD APIENTRY
NPGetCaps(
	DWORD Index)
{
	DWORD rc = 0;
	DbgPrintW(L"NPGetCaps %d\n", Index);
  
    switch (Index) {
	case WNNC_SPEC_VERSION:
		DbgPrintW(L"  WNNC_SPEC_VERSION\n");
		rc = WNNC_SPEC_VERSION51;
		break;
 
	case WNNC_NET_TYPE:
		DbgPrintW(L"  WNNC_NET_TYPE\n");
		rc = WNNC_NET_RDR2SAMPLE;
		break;
  
	case WNNC_DRIVER_VERSION:
		DbgPrintW(L"  WNNC_DRIVER_VERSION\n");
		rc = 1;
		break;

	case WNNC_CONNECTION:
		DbgPrintW(L"  WNC_CONNECTION\n");
		rc = WNNC_CON_GETCONNECTIONS |
			WNNC_CON_CANCELCONNECTION |
			WNNC_CON_ADDCONNECTION |
			WNNC_CON_ADDCONNECTION3;
		break;

	case WNNC_ENUMERATION:
		DbgPrintW(L"  WNNC_ENUMERATION\n");
		rc = WNNC_ENUM_LOCAL;
		break;
		
	case WNNC_START:
		DbgPrintW(L"  WNNC_START\n");
		rc = 1;
		break;

	case WNNC_USER:
		DbgPrintW(L"  WNNC_USER\n");
		rc = 0;
		break;
	case WNNC_DIALOG:
		DbgPrintW(L"  WNNC_DIALOG\n");
		rc = 0;
		break;
	case WNNC_ADMIN:
		DbgPrintW(L"  WNNC_ADMIN\n");
		rc = 0;
		break;
	default:
		DbgPrintW(L"  default\n");
		rc = 0;
		break;
    }

	return rc;
}

DWORD APIENTRY
NPLogonNotify(
	__in PLUID		LogonId,
	__in PCWSTR		AuthentInfoType,
	__in PVOID		AuthentInfo,
	__in PCWSTR		PreviousAuthentInfoType,
	__in PVOID		PreviousAuthentInfo,
	__in PWSTR		StationName,
	__in PVOID		StationHandle,
	__out PWSTR		*LogonScript)
{
	UNREFERENCED_PARAMETER(LogonId);
	UNREFERENCED_PARAMETER(AuthentInfoType);
	UNREFERENCED_PARAMETER(AuthentInfo);
	UNREFERENCED_PARAMETER(PreviousAuthentInfoType);
	UNREFERENCED_PARAMETER(PreviousAuthentInfo);
	UNREFERENCED_PARAMETER(StationName);
	UNREFERENCED_PARAMETER(StationHandle);

	DbgPrintW(L"NPLogonNotify\n");
	*LogonScript = NULL;
	return WN_SUCCESS;
}

DWORD APIENTRY
NPPasswordChangeNotify(
    __in LPCWSTR AuthentInfoType,
    __in LPVOID AuthentInfo,
	__in LPCWSTR PreviousAuthentInfoType,
	__in LPVOID RreviousAuthentInfo,
	__in LPWSTR StationName,
	__in PVOID StationHandle,
	__in DWORD ChangeInfo)
{
	UNREFERENCED_PARAMETER(AuthentInfoType);
	UNREFERENCED_PARAMETER(AuthentInfo);
	UNREFERENCED_PARAMETER(PreviousAuthentInfoType);
	UNREFERENCED_PARAMETER(RreviousAuthentInfo);
	UNREFERENCED_PARAMETER(StationName);
	UNREFERENCED_PARAMETER(StationHandle);
	UNREFERENCED_PARAMETER(ChangeInfo);

	DbgPrintW(L"NPPasswordChangeNotify\n");
	SetLastError(WN_NOT_SUPPORTED);
	return WN_NOT_SUPPORTED;
}


DWORD APIENTRY
NPAddConnection(
    __in LPNETRESOURCE NetResource,
	__in LPWSTR Password,
    __in LPWSTR UserName)
{
	DbgPrintW(L"NPAddConnection\n");
	return  NPAddConnection3(NULL, NetResource, Password, UserName, 0);
}

DWORD APIENTRY
NPAddConnection3(
    __in HWND WndOwner,
	__in LPNETRESOURCE NetResource,
	__in LPWSTR Password,
	__in LPWSTR UserName,
	__in DWORD Flags)
{
	DWORD status;
	WCHAR temp[MAX_PATH + 1];
	WCHAR local[3];

	UNREFERENCED_PARAMETER(WndOwner);
	UNREFERENCED_PARAMETER(Password);
	UNREFERENCED_PARAMETER(UserName);
	UNREFERENCED_PARAMETER(Flags);

	DbgPrintW(L"NPAddConnection3\n");
	DbgPrintW(L"  LocalName: %s\n", NetResource->lpLocalName);
	DbgPrintW(L"  RemoteName: %s\n", NetResource->lpRemoteName);
 
	ZeroMemory(local, sizeof(local));

	if (lstrlen(NetResource->lpLocalName) > 1 &&
		NetResource->lpLocalName[1] == L':') {
		local[0] = (WCHAR)toupper(NetResource->lpLocalName[0]);
		local[1] = L':';
		local[2] = L'\0';
	}

	if (QueryDosDevice(local, temp, MAX_PATH / 2)) {
		DbgPrintW(L"  WN_ALREADY_CONNECTED");
		status = WN_ALREADY_CONNECTED;
	} else {
		DbgPrintW(L"  WN_BAD_NETNAME");
		status = WN_BAD_NETNAME;
	}

	return status;
}

DWORD APIENTRY
NPCancelConnection(
     __in LPWSTR Name,
	 __in BOOL Force)
{
	DbgPrintW(L"NpCancelConnection %s %d\n", Name, Force);
	return WN_SUCCESS;
}


DWORD APIENTRY
NPGetConnection(
    __in LPWSTR LocalName,
	__out LPWSTR RemoteName,
	__inout LPDWORD BufferSize)
{
	DbgPrintW(L"NpGetConnection %s, %d\n", LocalName, *BufferSize);
	if (*BufferSize < sizeof(WCHAR) * 4) {
		*BufferSize = sizeof(WCHAR) * 4;
		return WN_MORE_DATA;
	}
	//if (NotConnected) {
	//	return WN_NOT_CONNECTED;
	//  return WN_NO_NETWORK;
	//}

	WCHAR drive[] = L" :\\";
	WCHAR tmpName[MAX_PATH];
	ZeroMemory(tmpName, MAX_PATH);
	drive[0] = LocalName[0];
	if (!GetVolumeInformation(drive, tmpName, MAX_PATH, NULL, NULL, NULL, NULL, 0)) {
		return WN_NO_NETWORK;
	}

	if (lstrlenW(tmpName) == 0) {
		lstrcpyW(RemoteName, drive);
	} else if (lstrlenW(tmpName) > (LONG)*BufferSize) {
		*BufferSize = lstrlenW(tmpName);
		return WN_MORE_DATA;
	}
	else {
		lstrcpyW(RemoteName, tmpName);
	}

	return WN_SUCCESS;
}


DWORD APIENTRY
NPOpenEnum(
     __in DWORD Scope,
	 __in DWORD Type,
	 __in DWORD Usage,
	 __in LPNETRESOURCE NetResource,
	 __in LPHANDLE Enum)
{
	DWORD status;

	UNREFERENCED_PARAMETER(Type);
	UNREFERENCED_PARAMETER(Usage);
	UNREFERENCED_PARAMETER(NetResource);

	DbgPrintW(L"NPOpenEnum\n");
    switch (Scope){
	case RESOURCE_CONNECTED:
        {
            *Enum = HeapAlloc(GetProcessHeap(),
							  HEAP_ZERO_MEMORY,
							  sizeof(ULONG));

            if (*Enum)
                status = WN_SUCCESS;
			else
                status = WN_OUT_OF_MEMORY;
        }
        break;
	case RESOURCE_CONTEXT:
	default:
		status  = WN_NOT_SUPPORTED;
		break;
    }
	return status;
}


DWORD APIENTRY
NPCloseEnum(
	__in HANDLE Enum)
{
	DbgPrintW(L"NpCloseEnum\n");
    HeapFree(GetProcessHeap(), 0, (PVOID)Enum);
	return WN_SUCCESS;
}


DWORD APIENTRY
NPGetResourceParent(
    __in LPNETRESOURCE NetResource,
	__in LPVOID Buffer,
	__in LPDWORD BufferSize)
{
	UNREFERENCED_PARAMETER(NetResource);
	UNREFERENCED_PARAMETER(Buffer);
	UNREFERENCED_PARAMETER(BufferSize);

	DbgPrintW(L"NPGetResourceParent\n");
	return WN_NOT_SUPPORTED;
}


DWORD APIENTRY
NPEnumResource(
     __in HANDLE Enum,
	 __in LPDWORD Count,
	 __in LPVOID Buffer,
	 __in LPDWORD BufferSize)
{
	UNREFERENCED_PARAMETER(Enum);
	UNREFERENCED_PARAMETER(Count);
	UNREFERENCED_PARAMETER(Buffer);
	UNREFERENCED_PARAMETER(BufferSize);

	DbgPrintW(L"NPEnumResource\n");
	return WN_NOT_SUPPORTED;
}

DWORD APIENTRY
NPGetResourceInformation(
    __in LPNETRESOURCE NetResource,
	__out LPVOID Buffer,
	__out LPDWORD BufferSize,
    __out LPWSTR *System)
{
	UNREFERENCED_PARAMETER(NetResource);
	UNREFERENCED_PARAMETER(Buffer);
	UNREFERENCED_PARAMETER(BufferSize);
	UNREFERENCED_PARAMETER(System);

	DbgPrintW(L"NPGetResourceInformation\n");
	return WN_NOT_SUPPORTED;
}


DWORD APIENTRY
NPGetUniversalName(
    __in LPCWSTR LocalPath,
	__in DWORD InfoLevel,
	__in LPVOID Buffer,
	__in LPDWORD BufferSize)
{
	UNREFERENCED_PARAMETER(LocalPath);
	UNREFERENCED_PARAMETER(InfoLevel);
	UNREFERENCED_PARAMETER(Buffer);
	UNREFERENCED_PARAMETER(BufferSize);

	DbgPrintW(L"NPGetUniversalName %s\n", LocalPath);
	return WN_NOT_SUPPORTED;
}

