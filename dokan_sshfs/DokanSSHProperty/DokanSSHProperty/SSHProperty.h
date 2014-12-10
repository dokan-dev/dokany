// SSHProperty.h : CSSHProperty の宣言

#pragma once
#include "resource.h"       // メイン シンボル
#include <string>
#include <list>


#if defined(_WIN32_WCE) && !defined(_CE_DCOM) && !defined(_CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA)
#error "DCOM の完全サポートを含んでいない Windows Mobile プラットフォームのような Windows CE プラットフォームでは、単一スレッド COM オブジェクトは正しくサポートされていません。ATL が単一スレッド COM オブジェクトの作成をサポートすること、およびその単一スレッド COM オブジェクトの実装の使用を許可することを強制するには、_CE_ALLOW_SINGLE_THREADED_OBJECTS_IN_MTA を定義してください。ご使用の rgs ファイルのスレッド モデルは 'Free' に設定されており、DCOM Windows CE 以外のプラットフォームでサポートされる唯一のスレッド モデルと設定されていました。"
#endif


// CSSHProperty

class ATL_NO_VTABLE CSSHProperty :
	public CComObjectRootEx<CComSingleThreadModel>,
	public CComCoClass<CSSHProperty, &CLSID_SSHProperty>,
	public IShellExtInit,
	public IShellPropSheetExt
{
public:
	CSSHProperty()
	{
	}


DECLARE_REGISTRY_RESOURCEID(IDR_SSHPROPERTY)

DECLARE_NOT_AGGREGATABLE(CSSHProperty)

BEGIN_COM_MAP(CSSHProperty)
	COM_INTERFACE_ENTRY(IShellExtInit)
	COM_INTERFACE_ENTRY(IShellPropSheetExt)
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

	// IShellPropSheetExt
    STDMETHODIMP AddPages(LPFNADDPROPSHEETPAGE, LPARAM);
    STDMETHODIMP ReplacePage(UINT, LPFNADDPROPSHEETPAGE, LPARAM) { return E_NOTIMPL; }

protected:
	string_list m_lsFiles;

};

OBJECT_ENTRY_AUTO(__uuidof(SSHProperty), CSSHProperty)
