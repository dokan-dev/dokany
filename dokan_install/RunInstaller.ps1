param(
	[switch] $d,
	[switch] $uninstall,
	
	[Parameter(Mandatory = $false)]
	[ValidateSet('x64', 'x86')]
	[string] $arch = 'x64'
)

if($d -eq $true)
{
	$debugStr = 'Debug'
}
else
{
	$debugStr = 'Release'
}

if($uninstall -eq $true)
{
	msiexec /L*v "DokanInstaller_$arch\bin\$debugStr\uninstall_log.txt" /x "DokanInstaller_$arg\bin\$debugStr\DokanInstaller_$arch.msi"
}
else
{
	msiexec /L*v "DokanInstaller_$arch\bin\$debugStr\install_log.txt" /i "DokanInstaller_$arch\bin\$debugStr\DokanInstaller_$arch.msi"
}