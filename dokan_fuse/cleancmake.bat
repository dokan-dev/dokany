@echo off
del CMakeCache.txt /Q /S >nul 2>&1
for /d /r . %%d in (CMakeFiles) do @if exist "%%d" rd /s/q "%%d"