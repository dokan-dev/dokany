
    Dokan ライブラリ

 Copyright(c) Hiroki Asakawa http://dokan-dev.net


■ Dokanライブラリとは

Windowsでファイルシステム，例えば FAT や NTFS の改良バージョンを作成し
ようと思ったときには，ファイルシステムドライバを作成する必要があります．
Windowsでカーネルモードで動作する，デバイスドライバを作成するのは非常に
難しく，簡単に作成することが出来ません．Dokanライブラリを使用することで
デバイスドライバを書かなくても，新しいファイルシステムを作成することが
出来ます．FUSE の Windows 版とも言えるライブラリです．


■ ライセンスについて

Dokanライブラリは複数のライセンスのプログラムから構成されています．

ユーザモードライブラリ (dokan.dll) LGPL
ドライバ (dokan.sys)               LGPL
補助プログラム (dokanctl.exe)      MIT
マウントサービス (mounter.exe)     MIT

それぞれのライセンスの詳細は，
LGPL license.lgpl.txt
GPL  license.gpl.txt
MIT  license.mit.txt
を参照して下さい．
ソースコードは http://dokan-dev.net/download から入手できます．

■ 動作環境

Windowx XP,2003,Vista,2008,7 x86版，Windows 2003,Vista,2008,7 x64版で動作します．


■ 動作の仕組み

Dokanライブラリは，ユーザモードのライブラリである dokan.dll とファイル
システムドライバである dokan.sys から構成されています．dokan.sysをイン
ストールすることで，Windows からは通常のファイルシステムが存在するよう
にに見えます．dokanファイルシステムに対するアクセスがあると，ユーザモー
ドにコールバックされます．例えば，エクスプローラでディレクトリを開いた
ときに，Windowsはファイルシステムに対して，ディレクトリ一覧を要求します．
この要求が，dokan.dll にコールバックされます．この要求に対して，適当な
ディレクトリ一覧をファイルシステムに戻すと，dokanファイルシステムは，そ
れをWindowsに返します．Dokanファイルシステムをプロキシとして介すること
で，ユーザモードの通常プログラムとしてファイルシステムが実装できるわけ
です．


■ ライブラリの構成とインストール

インストーラーを実行すると標準では以下のファイルがインストールされ，
ドライバーとサービスプログラムがシステムに登録されます．

SystemFolder\dokan.dll
    Dokanユーザモードライブラリ

SystemFolder\drivers\dokan.sys
    Dokanファイルシステムドライバ

ProgramFilesFolder\Dokan\DokanLibrary\mounter.exe
    Dokanマントサービス

ProgramFilesFolder\Dokan\DokanLibrary\dokanctl.exe
    Dokanコントロールプログラム

ProgramFilesFolder\Dokan\DokanLibrary\dokan.lib
    Dokanインポートライブラリ

ProgramFilesFolder\Dokan\DokanLibrary\dokan.h
    Dokanライブラリヘッダー

ProgramFilesFolder\Dokan\DokanLibrary\readme.txt
    このファイル


アンインストールはコントロールパネルのアプリケーションの追加と削除から
行ってください．アンインストール後再インストールするには再起動が必要に
なります．(必ず再起動してください)


■ ファイルシステムの作り方

dokan.h の DOKAN_OPERATIONS の各関数を実装し，DokanMain に渡すことでマ
ウントします．それぞれの関数はWindowsAPI風の引数を取ります．各関数のお
おまかな仕様は同名のWindowsAPIとほぼ同じです．
DOKAN_OPERATIONS の各関数は，任意のスレッドから呼ばれます．スレッドセー
フでないアプリケーションで問題になる場合があります．

次の順序で呼ばれます

1. CreateFile(OpenDirectory, OpenDirectory)
2. その他の関数
3. Cleanup
4. CloseFile

ファイルに対するアクセス(ディレクトリ一覧や，ファイルの属性の取得など）
の初めに，かならずCreateFile系の関数が呼ばれ，ファイルハンドルがクロー
ズされるとき(WindowsAPIのCloseFileが呼ばれたとき)にCleanupが呼ばれ，そ
の後CloseFile が呼ばれます．

それぞれの関数は正常終了した場合，0を，エラーが発生し場合は，Windowsの
エラーコードを -1倍したものを返してください．

それぞれの関数の最後の引数には DOKAN_FILE_INFO が渡されます．この構造体
は，次のように定義されています．

    typedef struct _DOKAN_FILE_INFO {

        ULONG64 Context; 
        ULONG64 DokanContext;
        ULONG   ProcessId;
        BOOL    IsDirectory;

    } DOKAN_FILE_INFO, *PDOKAN_FILE_INFO;

ユーザモードから同じファイルハンドルを利用してのアクセスにたいしては，
同じ DOKAN_FILE_INFO が関数に渡されてきます．この構造体は，CreateFile
で生成され，CloseFileで解放されます．DokanFileInfo->Context はファイル
システムが自由に使用できる変数です．一連のファイルアクセスに対して，保
存されるので，ファイルハンドルなどを保存するのに利用できます．
DokanFileInfo->DokanContext は内部処理用です．さわらないでください．
DokanFileInfo->ProcessId は IOリクエストを生成したプロセスIDです．
DokanFileInfo->IsDirectory はディレクトリに対するアクセスだと TRUE がセッ
トされています(例外あり後述)．

    int (*CreateFile) (
        LPCWSTR,      // FileName
        DWORD,        // DesiredAccess
        DWORD,        // ShareMode
        DWORD,        // CreationDisposition
        DWORD,        // FlagsAndAttributes
        PDOKAN_FILE_INFO);

    int (*OpenDirectory) (
        LPCWSTR,          // FileName
        PDOKAN_FILE_INFO);

    int (*CreateDirectory) (
        LPCWSTR,          // FileName
        PDOKAN_FILE_INFO);

CreateFile ファイルの新規作成，開くなどOpenDirectory ディレクトリを開く
CreateDirectory ディレクトリを作成するそれぞれの関数は，ファイルへのア
クセス開始時に呼ばれます．関数の仕様は，WindowsAPIに似せてあります．
CreateFile のDesiredAccess ShareMode CreationDisposition
FlagsAndAttributes については，MSDN で CreateFile を参照してください．
ディレクトリに対するアクセスの時には，OpenDirectory または，
CreateDirectory が呼ばれます．その場合，DokanFileInfo->IsDirectory が
TRUE になっています．ディレクトリに対するアクセスで有るのにも関わらず，
CreateFile が呼ばれることもあります．その場合は，
DokanFileInfo->IsDirectory は FALSE がセットされています．ディレクトリ
の属性を取得する場合などに OpenDirectory ではなく，CreateFileが呼ばれる
ようです．ディレクトリに対するアクセスなのにも関わらず，CreateFile が呼
ばれた場合は，必ず DokanFileInfo->IsDirectory にTRUEをセットしてから
returnしてください．正しくセットされていないと，Dokanライブラリは，その
アクセスがディレクトリに対するアクセスかどうか判断できず，Dokanファイル
システムで Windows に対して正確な情報を返すことが出来なくなります．

CreateFile で CreationDisposition が CREATE_ALWAYS もしくは
OPEN_ALWAYS の場合で，ファイルがすでに存在していた場合は，0ではなく，
ERROR_ALREADY_EXISTS(183) (正の値) を返してください．


    int (*Cleanup) (
        LPCWSTR,      // FileName
        PDOKAN_FILE_INFO);

    int (*CloseFile) (
        LPCWSTR,      // FileName
        PDOKAN_FILE_INFO);

Cleanup はユーザが WindowsAPI の CloseHandle を呼んだときに呼ばれます．
CreateFile の時に，ファイルを開き，ファイルハンドルを例えば，
DokanFileInfo->Context に保存している場合，CloseFile時ではなく，
Cleanup時にそのファイルハンドルを閉じるべきです．ユーザが WindowsAPI の
CloseHandle を呼んで，その後同じファイルを開いた場合，Cleanup が呼ばれ
ても CloseFile が新たにファイルを開く前に呼ばれない場合があります．ファ
イルシステムがファイルを開いたままにしている場合，ファイルの共有違反で
再度開けなくなるかもしれません．【注意】ユーザがファイルをメモリマップ
ドファイルとして開いている場合，Cleanup が呼ばれた後に WriteFile や
ReadFile が呼ばれる場合があります．この場合にも正常に読み込み書き込みが
できるようにするべきです．


    int (*FindFiles) (
        LPCWSTR,           // PathName
        PFillFindData,     // call this function with PWIN32_FIND_DATAW
        PDOKAN_FILE_INFO); //  (see PFillFindData definition)


    // You should implement FindFires or FindFilesWithPattern
    int (*FindFilesWithPattern) (
        LPCWSTR,           // PathName
        LPCWSTR,           // SearchPattern
        PFillFindData,     // call this function with PWIN32_FIND_DATAW
        PDOKAN_FILE_INFO);


FindFiles と FindFilesWithPattern はディレクトリ一覧を取得するときに呼
ばれます．ディレクトリ情報を WIN32_FIND_DATAW に格納し，引数に渡される
FillFindData 関数ポインタを通じて，FillFindData(&win32FindDataw,
DokanFileInfo) を１エントリごとに呼び出してください．
FindFilesWithPattern はディレクトリの検索パターン付きで呼び出されます．
Windows は，ワイルドカードの展開をシェルではなく，ファイルシステムで行
います．ワイルドカード展開を制御したい場合は，FindFilesWithPattern を定
義してください．FindFiles はワイルドカード制御を Dokanライブラリが行い
ます．dokan.dll は DokanIsNameInExpression をエクスポートしており，ワイ
ルドカードのマッチングに利用できます．


■ マウント

    typedef struct _DOKAN_OPTIONS {
        WCHAR   DriveLetter; // driver letter to be mounted
        ULONG   ThreadCount; // number of threads to be used
        UCHAR   DebugMode; // ouput debug message
        UCHAR   UseStdErr; // ouput debug message to stderr
        UCHAR   UseAltStream; // use alternate stream

    } DOKAN_OPTIONS, *PDOKAN_OPTIONS;

    int DOKANAPI DokanMain(
        PDOKAN_OPTIONS    DokanOptions,
        PDOKAN_OPERATIONS DokanOperations);

DokanOptions には，Dokan の実行オプション，DokanOptions に各ディスパッチ
関数の関数ポインタを指定して，DokanMain を呼びます．DokanMain はアンマウ
ントするまで制御を返しません．また，各ディスパッチ関数は，DokanMain を呼
んだスレッドコンテキストと異なる複数のスレッドコンテキストから呼ばれます．
ディスパッチ関数は，スレッドセーフになるようにして下さい．

DOKAN_OPTIONS
   DriveLetter: マウントするドライブ
   ThreadCount: Dokan ライブラリ内部で使用するスレッドの個数．0を指定す
                ればデフォルト値が使われます．デバッグ時に 1 を指定する
                とデバッグしやすくなります．
   DebugMode  : 1 にセットすると，デバッグメッセージがデバッグ出力に出力
                されます．
   UseStdErr  : 1 にセットすると，デバッグメッセージが標準エラー出力に出
                力されます．
   UseAltStream : 代替ストリーム(alternate stream, 副ストリーム)を使用し
                  ます．

DokanMain はマウントに失敗すると次のようなエラーコードを返します．

    #define DOKAN_SUCCESS                0
    #define DOKAN_ERROR                 -1 /* General Error */
    #define DOKAN_DRIVE_LETTER_ERROR    -2 /* Bad Drive letter */
    #define DOKAN_DRIVER_INSTALL_ERROR  -3 /* Can't install driver */
    #define DOKAN_START_ERROR           -4 /* Driver something wrong */
    #define DOKAN_MOUNT_ERROR           -5 /* Can't assign a drive letter */



■ アンマウント

DokanUnmount を呼べばアンマウントできます．プログラムがハングした場合や，
エクスプローラがハングした場合は，アンマウントを行えば大抵の場合元に戻
ります．

    > dokanctl.exe /u DriveLetter

で unmount を行えます．



■ そのほか

Dokanライブラリもしくは，このライブラリを利用して作成したファイルシステ
ムの不具合のために，ブルースクリーンになることがあります．ファイルシス
テムの開発には，Virtual Machineを利用することを強くお勧めします．
