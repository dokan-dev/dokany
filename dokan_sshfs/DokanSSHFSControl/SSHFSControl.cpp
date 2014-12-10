// SSHFSControl.cpp : CSSHFSControl ‚ÌŽÀ‘•

#include "stdafx.h"
#include "SSHFSControl.h"


// CSSHFSControl

static BOOL
CheckMount2(LPCWSTR FileName)
{
	std::wstring file = std::wstring(FileName) + L":SSHFSProperty.Permission";


	HANDLE handle = CreateFile(
					file.c_str(),
					FILE_GENERIC_READ,
					FILE_SHARE_READ,
					0,
					OPEN_EXISTING,
					0,
					0); 

	if (handle == INVALID_HANDLE_VALUE) {
		return FALSE;
	}
	CloseHandle(handle);
	return TRUE;
}


// CSSHFSControl

STDMETHODIMP CSSHFSControl::Initialize ( 
  LPCITEMIDLIST pidlFolder,
  LPDATAOBJECT pDataObj,
  HKEY hProgID )
{
	TCHAR     szFile[MAX_PATH];
	FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT,
						-1, TYMED_HGLOBAL };
	STGMEDIUM stg = { TYMED_HGLOBAL };
	HDROP     hDrop;
	BOOL	  mounted = FALSE;

	if (FAILED(pDataObj->GetData(&fmt, &stg)))
		return E_INVALIDARG;

	hDrop = (HDROP)GlobalLock(stg.hGlobal);
	if (NULL == hDrop)
		return E_INVALIDARG;

	UINT uNumFiles = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);
	HRESULT hr = S_OK;
  
	if (0 == uNumFiles) {
		GlobalUnlock ( stg.hGlobal );
		ReleaseStgMedium ( &stg );
		return E_INVALIDARG;
	}
 

	uNumFiles = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);

	for (UINT uFile = 0; uFile < uNumFiles; uFile++ ) {

		// Get the next filename.
		if (0 == DragQueryFile(hDrop, uFile, szFile, MAX_PATH))
			continue;

		//OutputDebugStringW(L"SSHFSControl:");
		//OutputDebugStringW(szFile);
		//OutputDebugStringW(L"\n");

		if (!mounted && CheckMount2(szFile))
			mounted = TRUE;

		// Add the filename to our list o' files to act on.
		m_lsFiles.push_back(szFile);
	}


	GlobalUnlock(stg.hGlobal);
	ReleaseStgMedium(&stg);
 
	return (m_lsFiles.size() > 0 && mounted) ? S_OK : E_FAIL;
}

HRESULT CSSHFSControl::QueryContextMenu(
	HMENU hmenu, UINT uMenuIndex, UINT uidFirstCmd,
	UINT uidLastCmd, UINT uFlags )
{
	// If the flags include CMF_DEFAULTONLY then we shouldn't do anything.

	if (uFlags & CMF_DEFAULTONLY)
		return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
 
	
	InsertMenu(hmenu, uMenuIndex, MF_BYPOSITION,
               uidFirstCmd, _T("SSHFS Clear Cache"));
 
	return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 1);
}


HRESULT CSSHFSControl::GetCommandString(
	UINT idCmd, UINT uFlags, UINT* pwReserved,
	LPSTR pszName, UINT cchMax)
{
	USES_CONVERSION;
 
	// Check idCmd, it must be 0 since we have only one menu item.

	if ( 0 != idCmd )
		return E_INVALIDARG;
 
	// If Explorer is asking for a help string, copy our string into the

	// supplied buffer.

	if (uFlags & GCS_HELPTEXT) {
		LPCTSTR szText = _T("Dokan SSHFS Clear Cache");
 
		if (uFlags & GCS_UNICODE) {
			// We need to cast pszName to a Unicode string, and then use the

			// Unicode string copy API.

			lstrcpynW((LPWSTR)pszName, T2CW(szText), cchMax);
		
		} else {
			// Use the ANSI string copy API to return the help string.

			lstrcpynA(pszName, T2CA(szText), cchMax);
		}
 
		return S_OK;
	}
 
	return E_INVALIDARG;
}


HRESULT CSSHFSControl::InvokeCommand(
	LPCMINVOKECOMMANDINFO pCmdInfo)
{
	// If lpVerb really points to a string, ignore this function call and bail out.

	if (0 != HIWORD(pCmdInfo->lpVerb))
		return E_INVALIDARG;
 
	// Get the command index - the only valid one is 0.

	switch (LOWORD(pCmdInfo->lpVerb)) {
	case 0:
		{
			for (string_list::const_iterator it = m_lsFiles.begin();
				it != m_lsFiles.end(); ++it) {
				
				std::wstring filename = *it + L":SSHFSProperty.Cache";
				
				HANDLE handle = CreateFile(
					filename.c_str(),
					FILE_GENERIC_WRITE,
					FILE_SHARE_WRITE,
					0,
					OPEN_EXISTING,
					0,
					0); 

				if (handle == INVALID_HANDLE_VALUE)
					continue;

				CloseHandle(handle);
			}
			return S_OK;
		}
	break;
 
	default:
		return E_INVALIDARG;
	break;
	}
}
