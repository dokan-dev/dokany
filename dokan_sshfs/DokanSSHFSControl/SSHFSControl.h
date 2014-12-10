// SSHFSControl.h : CSSHFSControl の宣言

#pragma once
#include "resource.h"       // メイン シンボル

#include <shlobj.h>
#include <comdef.h>
#include <string>
#include <list>

#include "DokanSSHFSControl.h"


#if defined(_WIN32_WCE) && !defined(_CE_DCOM) && !defined(_CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA)
#error "DCOM の完全サポートを含んでいない Windows Mobile プラットフォームのような Windows CE プラットフォームでは、単一スレッド COM オブジェクトは正しくサポートされていません。ATL が単一スレッド COM オブジェクトの作成をサポートすること、およびその単一スレッド COM オブジェクトの実装の使用を許可することを強制するには、_CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA を定義してください。ご使用の rgs ファイルのスレッド モデルは 'Free' に設定されており、DCOM Windows CE 以外のプラットフォームでサポートされる唯一のスレッド モデルと設定されていました。"
#endif



// CSSHFSControl

class ATL_NO_VTABLE CSSHFSControl :
	public CComObjectRootEx<CComSingleThreadModel>,
	public CComCoClass<CSSHFSControl, &CLSID_SSHFSControl>,
	public IShellExtInit,
	public IContextMenu
{
public:
	CSSHFSControl()
	{
	}

DECLARE_REGISTRY_RESOURCEID(IDR_SSHFSCONTROL)

DECLARE_NOT_AGGREGATABLE(CSSHFSControl)

BEGIN_COM_MAP(CSSHFSControl)
	COM_INTERFACE_ENTRY(IShellExtInit)
	COM_INTERFACE_ENTRY(IContextMenu)
END_COM_MAP()



	DECLARE_PROTECT_FINAL_CONSTRUCT()

	HRESULT FinalConstruct()
	{
		return S_OK;
	}

	void FinalRelease()
	{
	}

	typedef std::list< std::wstring > string_list;

public:
	// IShellExtInit
    STDMETHODIMP Initialize(LPCITEMIDLIST, LPDATAOBJECT, HKEY);

	// IContextMenu
	STDMETHODIMP GetCommandString(UINT, UINT, UINT*, LPSTR, UINT);
	STDMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO);
	STDMETHODIMP QueryContextMenu(HMENU, UINT, UINT, UINT, UINT);

protected:
	string_list m_lsFiles;

};

OBJECT_ENTRY_AUTO(__uuidof(SSHFSControl), CSSHFSControl)
