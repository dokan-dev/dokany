del CMakeCache.txt /Q /S
for /d /r . %%d in (CMakeFiles) do @if exist "%%d" rd /s/q "%%d"