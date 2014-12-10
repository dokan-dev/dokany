// SSHProperty.cpp : CSSHProperty ‚ÌŽÀ‘•

#include "stdafx.h"
#include "DokanSSHProperty.h"
#include "SSHProperty.h"
#include "DokanSSHPropertyModule.h"
#include "..\..\..\dokan\dokanc.h"

extern CDokanSSHPropertyModule _AtlModule;

static BOOL g_UseStdErr = FALSE;

static BOOL
CheckMount(WCHAR drive)
{
	HANDLE pipe;
	DWORD readBytes;
	DWORD pipeMode;

	DOKAN_CONTROL control;
	ZeroMemory(&control, sizeof(DOKAN_CONTROL));

	control.Type = DOKAN_CONTROL_CHECK;
	control.Check.Drive = drive;

	DokanDbgPrintW(L"CheckMount Drive: %c\n", drive);

	pipe = CreateFile(DOKAN_CONTROL_PIPE,
		GENERIC_READ|GENERIC_WRITE,
		0, NULL, OPEN_EXISTING, 0, NULL);

	if (pipe == INVALID_HANDLE_VALUE) {
		if (GetLastError() == ERROR_ACCESS_DENIED) {
			DokanDbgPrintW(L"failed to connect DokanMounter service: access denied\n");
		} else {
			DokanDbgPrintW(L"failed to connect DokanMounter service: %d\n", GetLastError());
		}
		return FALSE;
	}

	pipeMode = PIPE_READMODE_MESSAGE|PIPE_WAIT;

	if(!SetNamedPipeHandleState(pipe, &pipeMode, NULL, NULL)) {
		DokanDbgPrintW(L"failed to set named pipe state\n");
		return FALSE;
	}


	if(!TransactNamedPipe(pipe, &control, sizeof(DOKAN_CONTROL),
		&control, sizeof(DOKAN_CONTROL), &readBytes, NULL)) {
		
		DokanDbgPrintW(L"failed to transact named pipe\n");
	}

	if(control.Status != DOKAN_CONTROL_FAIL)
		return TRUE;
	else
		return FALSE;
}

static BOOL
CheckMount2(LPCWSTR FileName)
{
	std::wstring file = std::wstring(FileName) + L":SSHFSProperty.Permission";

	DokanDbgPrintW(L"Check SSHFS file %s\n", file.c_str());

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


BOOL
OnInitDialog(HWND hwnd, LPARAM lParam)
{        
	CSSHProperty::string_list*	files;
	PROPSHEETPAGE*	ppsp = (PROPSHEETPAGE*)lParam;

	files = (CSSHProperty::string_list*)ppsp->lParam;

	if (!files)
		return FALSE;

	// Store the filename in this window's user data area, for later use.
    SetWindowLong(hwnd, GWL_USERDATA, (LONG)files);

	CSSHProperty::string_list::iterator it = files->begin();
	
	int reads[3];
	int writes[3];
	int execs[3];

	ZeroMemory(reads, sizeof(reads));
	ZeroMemory(writes, sizeof(writes));
	ZeroMemory(execs, sizeof(execs));

	for (; it != files->end(); ++it) {
		std::wstring file = *it;
		file += L":SSHFSProperty.Permission";

		HANDLE hFile = CreateFile(
						file.c_str(),
						GENERIC_READ,
						FILE_SHARE_READ,
						NULL,
						OPEN_EXISTING,
						0,
						NULL);

		if(hFile == INVALID_HANDLE_VALUE)
			continue;

		char buffer[32];
		ZeroMemory(buffer, sizeof(buffer));

		DWORD readBytes = 0;
		if (ReadFile(hFile, buffer, sizeof(buffer), &readBytes, NULL)) {
			int p = atoi(buffer);
			DokanDbgPrintW(L"%s, %d\n", file.c_str(), p);

			
			for(int i=0; i<3; ++i) {
				p = buffer[i] - '0';
				if (p & 0x1)
					execs[i]++;
				if (p & 0x2)
					writes[i]++;
				if (p & 0x4)
					reads[i]++;
			}
		}

		CloseHandle(hFile);
	}

	int count = (int)files->size();

	CheckDlgButton(hwnd, IDC_W_READ,
					reads[0] == 0 ? BST_UNCHECKED :
					(reads[0] == count ? BST_CHECKED : BST_INDETERMINATE));
	CheckDlgButton(hwnd, IDC_W_WRITE,
					writes[0] == 0 ? BST_UNCHECKED :
					(writes[0] == count ? BST_CHECKED : BST_INDETERMINATE));
	CheckDlgButton(hwnd, IDC_W_EXEC,
					execs[0] == 0 ? BST_UNCHECKED :
					(execs[0] == count ? BST_CHECKED : BST_INDETERMINATE));

	CheckDlgButton(hwnd, IDC_G_READ,
					reads[1] == 0 ? BST_UNCHECKED :
					(reads[1] == count ? BST_CHECKED : BST_INDETERMINATE));
	CheckDlgButton(hwnd, IDC_G_WRITE,
					writes[1] == 0 ? BST_UNCHECKED :
					(writes[1] == count ? BST_CHECKED : BST_INDETERMINATE));
	CheckDlgButton(hwnd, IDC_G_EXEC,
					execs[1] == 0 ? BST_UNCHECKED :
					(execs[1] == count ? BST_CHECKED : BST_INDETERMINATE));

	CheckDlgButton(hwnd, IDC_O_READ,
					reads[2] == 0 ? BST_UNCHECKED :
					(reads[2] == count ? BST_CHECKED : BST_INDETERMINATE));
	CheckDlgButton(hwnd, IDC_O_WRITE,
					writes[2] == 0 ? BST_UNCHECKED :
					(writes[2] == count ? BST_CHECKED : BST_INDETERMINATE));
	CheckDlgButton(hwnd, IDC_O_EXEC,
					execs[2] == 0 ? BST_UNCHECKED :
					(execs[2] == count ? BST_CHECKED : BST_INDETERMINATE));


	return FALSE; // Take the default focus handling.
}


BOOL
OnApply(HWND hwnd, PSHNOTIFY* phdr)
{
	CSSHProperty::string_list* files = (CSSHProperty::string_list*)GetWindowLong(hwnd, GWL_USERDATA);
	HANDLE   hFile;

	CSSHProperty::string_list::iterator it = files->begin();

	for(; it != files->end(); ++it) {
		std::wstring file = *it + L":SSHFSProperty.Permission";
		hFile = CreateFile(
					file.c_str(),
					GENERIC_READ,
					FILE_SHARE_READ,
					NULL,
					OPEN_EXISTING,
					0,
					NULL);

		if (hFile == INVALID_HANDLE_VALUE)
			continue;

		char buffer[32];
		DWORD readBytes = 0;
		ZeroMemory(buffer, sizeof(buffer));
		if (!ReadFile(hFile, buffer, sizeof(buffer), &readBytes, NULL)) {
			CloseHandle(hFile);
			continue;
		}
		CloseHandle(hFile);

		int owner = buffer[0] - '0';
		UINT state;
		state = IsDlgButtonChecked(hwnd, IDC_W_EXEC);
		if (state == BST_CHECKED)
			owner |= 0x1;
		if (state == BST_UNCHECKED)
			owner &= ~0x1;
		
		state = IsDlgButtonChecked(hwnd, IDC_W_WRITE);
		if (state == BST_CHECKED)
			owner |= 0x2;
		if (state == BST_UNCHECKED)
			owner &= ~0x2;

		state = IsDlgButtonChecked(hwnd, IDC_W_READ);
		if (state == BST_CHECKED)
			owner |= 0x4;
		if (state == BST_UNCHECKED)
			owner &= ~0x4;


		int group = buffer[1] - '0';
		state = IsDlgButtonChecked(hwnd, IDC_G_EXEC);
		if (state == BST_CHECKED)
			group |= 0x1;
		if (state == BST_UNCHECKED)
			group &= ~0x1;
		
		state = IsDlgButtonChecked(hwnd, IDC_G_WRITE);
		if (state == BST_CHECKED)
			group |= 0x2;
		if (state == BST_UNCHECKED)
			group &= ~0x2;

		state = IsDlgButtonChecked(hwnd, IDC_G_READ);
		if (state == BST_CHECKED)
			group |= 0x4;
		if (state == BST_UNCHECKED)
			group &= ~0x4;


		int other = buffer[2] - '0';
		state = IsDlgButtonChecked(hwnd, IDC_O_EXEC);
		if (state == BST_CHECKED)
			other |= 0x1;
		if (state == BST_UNCHECKED)
			other &= ~0x1;
		
		state = IsDlgButtonChecked(hwnd, IDC_O_WRITE);
		if (state == BST_CHECKED)
			other |= 0x2;
		if (state == BST_UNCHECKED)
			other &= ~0x2;

		state = IsDlgButtonChecked(hwnd, IDC_O_READ);
		if (state == BST_CHECKED)
			other |= 0x4;
		if (state == BST_UNCHECKED)
			other &= ~0x4;


		char newpermission[32];
		ZeroMemory(newpermission, sizeof(newpermission));
		newpermission[0] = owner + '0';
		newpermission[1] = group + '0';
		newpermission[2] = other + '0';

		bool changed = false;
		for(int i=0; i<3; ++i)
			if(buffer[i] != newpermission[i])
				changed = true;

		buffer[3] = '\0';
		DokanDbgPrint("SSHFSProperty: %s %s -> %s\n",
			file.c_str(), buffer, newpermission);

		if (changed) {
			hFile = CreateFile(
				file.c_str(),
				GENERIC_WRITE,
				FILE_SHARE_WRITE,
				NULL,
				OPEN_EXISTING,
				0,
				NULL);

			if (hFile == INVALID_HANDLE_VALUE)
				continue;

			DWORD writtenBytes = 0;
			WriteFile(hFile, newpermission, 3, &writtenBytes, NULL);
			CloseHandle(hFile);
		}

	}

    // Return PSNRET_NOERROR to allow the sheet to close if the user clicked OK.
    SetWindowLong(hwnd, DWL_MSGRESULT, PSNRET_NOERROR);
    return TRUE;
}


BOOL
CALLBACK
PropPageDlgProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	BOOL bRet = FALSE;

	switch (uMsg) {
	case WM_INITDIALOG:
		bRet = OnInitDialog(hwnd, lParam);
		break;

	case WM_NOTIFY:
		{
			NMHDR* phdr = (NMHDR*)lParam;

			switch (phdr->code) {
			case PSN_APPLY:
				bRet = OnApply(hwnd, (PSHNOTIFY*)phdr);
				break;
			}
		}
		break;
	}

    return bRet;
}


UINT
CALLBACK
PropPageCallbackProc(HWND hwnd, UINT uMsg, LPPROPSHEETPAGE ppsp)
{
    if (PSPCB_RELEASE == uMsg)
        free ( (void*) ppsp->lParam );

    return 1;   // used for PSPCB_CREATE - let the page be created
}


STDMETHODIMP
CSSHProperty::Initialize (
	LPCITEMIDLIST pidlFolder, LPDATAOBJECT pDataObj, HKEY hProgID )
{
	TCHAR     szFile[MAX_PATH];
	UINT      uNumFiles;
	HDROP     hdrop;
	FORMATETC etc = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
	STGMEDIUM stg;
	INITCOMMONCONTROLSEX iccex = { sizeof(INITCOMMONCONTROLSEX), ICC_DATE_CLASSES };
	BOOL	  mounted = FALSE;

	// Init the common controls.
	InitCommonControlsEx ( &iccex );

	// Read the list of folders from the data object.  They're stored in HDROP
	// form, so just get the HDROP handle and then use the drag 'n' drop APIs
	// on it.
	if (FAILED(pDataObj->GetData(&etc, &stg)))
		return E_INVALIDARG;

	// Get an HDROP handle.
	hdrop = (HDROP)GlobalLock(stg.hGlobal);

	if (NULL == hdrop)
	{
		ReleaseStgMedium(&stg);
		return E_INVALIDARG;
	}

	// Determine how many files are involved in this operation.
	uNumFiles = DragQueryFile(hdrop, 0xFFFFFFFF, NULL, 0);

	for (UINT uFile = 0; uFile < uNumFiles; uFile++ ) {

		// Get the next filename.
		if (0 == DragQueryFile(hdrop, uFile, szFile, MAX_PATH))
			continue;


		if (!mounted && CheckMount2(szFile))
			mounted = TRUE;

		// Skip over directories.  We *could* handle directories, since they
		// keep the creation time/date, but I'm just choosing not to do so
		// in this example project.
		//if ( PathIsDirectory ( szFile ) )
		//	continue;

		// Add the filename to our list o' files to act on.
		DokanDbgPrintW(L"add %s\n", szFile);
		m_lsFiles.push_back(szFile);
	}   // end for

	// Release resources.
	GlobalUnlock(stg.hGlobal);
	ReleaseStgMedium(&stg);

	// Check how many files were selected.  If the number is greater than the
	// maximum number of property pages, truncate our list.
	if (m_lsFiles.size() > MAXPROPPAGES)
		m_lsFiles.resize(MAXPROPPAGES);

	// If we found any files we can work with, return S_OK.  Otherwise,
	// return E_FAIL so we don't get called again for this right-click
	// operation.
	DokanDbgPrintW(mounted ? L"mounted\n" : L"not mounted\n");
	return (m_lsFiles.size() > 0 && mounted) ? S_OK : E_FAIL;
}


STDMETHODIMP
CSSHProperty::AddPages(
    LPFNADDPROPSHEETPAGE lpfnAddPageProc, LPARAM lParam)
{
	PROPSHEETPAGE  psp;
	HPROPSHEETPAGE hPage;

	// Set up the PROPSHEETPAGE struct.
	ZeroMemory(&psp, sizeof(PROPSHEETPAGE));

	psp.dwSize      = sizeof(PROPSHEETPAGE);
	psp.dwFlags     = PSP_USEREFPARENT | PSP_USETITLE | PSP_DEFAULT |
			PSP_USEICONID | PSP_USECALLBACK;
	psp.hInstance   = _AtlBaseModule.GetResourceInstance();
	psp.pszTemplate = MAKEINTRESOURCE(IDD_SSH_PROPERTY);
	psp.pszIcon     = NULL;//MAKEINTRESOURCE(IDI_TAB_ICON);
	psp.pszTitle	= TEXT("Permission");
	psp.pfnDlgProc  = PropPageDlgProc;
	psp.lParam      = (LPARAM)new string_list(m_lsFiles);
	psp.pfnCallback = PropPageCallbackProc;
	psp.pcRefParent = (UINT*) &_AtlModule.m_nLockCnt;

	// Create the page & get a handle.
	hPage = CreatePropertySheetPage(&psp);

	if (NULL != hPage) {
		// Call the shell's callback function, so it adds the page to
		// the property sheet.
		if (!lpfnAddPageProc(hPage, lParam))
			DestroyPropertySheetPage(hPage);
	}

	return S_OK;
}


