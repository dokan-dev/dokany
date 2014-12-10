========================================================================
ATL (ACTIVE TEMPLATE LIBRARY) : DokanSSHFSControl プロジェクトの概要
========================================================================

この DokanSSHFSControl プロジェクトは、ユーザーがダイナミック リンク ライ
ブラリ (DLL: Dynamic Link Library) を作成するための開始点として使用するために、
AppWizard によって作成されました。


このファイルには、プロジェクトを構成する各ファイルの内容の概略が記述されていま
す。

DokanSSHFSControl.vcproj
    これは、アプリケーション ウィザードで生成される VC++ プロジェクトのメインの
    プロジェクト ファイルです。
    ファイルを生成した Visual C++ のバージョン情報と、アプリケーション ウィザー
    ドで選択したプラットフォーム、構成、およびプロジェクトの機能に関する情報が
    記述されています。

DokanSSHFSControl.idl
    このファイルには、プロジェクトで定義されるタイプ ライブラリの IDL 定義、
    インターフェイス、およびコクラスが含まれます。
    このファイルは MIDL コンパイラによって処理され、次のものが生成されます。
        C++ のインターフェイス定義と GUID 宣言 (DokanSSHFSControl.h)
        GUID 定義                              (DokanSSHFSControl_i.c)
        タイプ ライブラリ                      (DokanSSHFSControl.tlb)
        マーシャリング コード                  (DokanSSHFSControl_p.c および dlldata.c)

DokanSSHFSControl.h
    このファイルには、C++ のインターフェイス定義および 
    DokanSSHFSControl.idl. で定義される項目の GUID 宣言が含まれます。
    コンパイル中に MIDL によって再生成されます。

DokanSSHFSControl.cpp
    このファイルには、オブジェクト マップおよび DLL のエクスポートの実装が
    含まれます。

DokanSSHFSControl.rc
    これは、プログラムで使用する Microsoft Windows の全リソースの一覧です。

DokanSSHFSControl.def
    このモジュール定義ファイルは、DLL に必要なエクスポートに関する情報をリンカ
    に提供します。次のエクスポート情報が含まれています。
        DllGetClassObject  
        DllCanUnloadNow    
        GetProxyDllInfo    
        DllRegisterServer
        DllUnregisterServer

/////////////////////////////////////////////////////////////////////////////
その他の標準ファイル:

StdAfx.h, StdAfx.cpp
    これらのファイルは、コンパイル済みヘッダー (PCH) ファイル 
    DokanSSHFSControl.pch とプリコンパイル済み型ファイル StdAfx.obj 
    をビルドするために使用します。

Resource.h
    これは、リソース ID を定義する標準のヘッダー ファイルです。

/////////////////////////////////////////////////////////////////////////////
プロキシ/スタブ DLL プロジェクトおよびモジュール定義ファイル:

DokanSSHFSControlps.vcproj
    このファイルは、必要に応じてプロキシ/スタブ DLL を構築するためのプロジェク
    ト ファイルです。
        メイン プロジェクトの IDL ファイルにインターフェイスが少なくとも 1 つ
        含まれていること、およびプロキシ/スタブ DLL をビルドする前に IDL 
        ファイルをコンパイルすることが必要です。このプロセスによって、
        dlldata.c, プロキシ/スタブ DLL をビルドするために必要な 
	DokanSSHFSControl_i.c と DokanSSHFSControl_p.c が生成され
        ます。

DokanSSHFSControlps.def
    このモジュール定義ファイルは、プロキシ/スタブに必要なエクスポートに関する
    情報をリンカに提供します。

/////////////////////////////////////////////////////////////////////////////




