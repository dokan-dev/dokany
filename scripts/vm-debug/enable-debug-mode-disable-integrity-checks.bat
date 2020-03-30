REM Enable Windows debug mode
bcdedit -debug on
REM Use selfsigned certificats
bcdedit -set loadoptions DDISABLE_INTEGRITY_CHECKS
bcdedit -set TESTSIGNING ON