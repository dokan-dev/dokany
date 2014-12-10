
    Dokan .NET バインディング

 Copyright(c) Hiroki Asakawa http://dokan-dev.net


■ このソフトウェアについて

Windows 用の仮想ファイルシステム Dokan の .NET バインディングです．
各種.NET言語で Windows用のファイルシステムを記述することが可能です．
実行には Dokan ライブラリが必要です．


■ ライセンスについて

MIT ライセンスに従います．license.txt をご覧ください．


■ 動作環境

.NET Framework 2.0 以上


■ ファイルシステムの作成方法

interface DokanOperationsを実装し，DokanNet.DokanMainを呼ぶことでマウントします．
DokanNet.DokanMain はアンマウントするまで制御を戻しません．注意点や関数の仕様は
Dokanライブラリとほぼ同じです．.NETバインディングでは，引数が.NET Frameworkで使
われている構造体やクラスに変更されています．詳しくはDokanライブラリのreadme.txt
ファイルを参照してください．また，sampleディレクトリ以下のサンプルプログラムが
あります．アプリケーションの実行にはAdministator権限が必要です．


■ アンマウント

次のコマンドを実行するか，アプリケーションからDokanNet.Unmountを呼びます．

   > dokanctl.exe /u DriveLetter



