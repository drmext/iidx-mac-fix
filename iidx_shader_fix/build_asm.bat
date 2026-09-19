@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"

cl /nologo /O2 /EHs- /D_CRT_SECURE_NO_WARNINGS asm_blob.cpp /Fe:asm_blob.exe /link /MACHINE:X64 ole32.lib || exit /b 1
asm_blob.exe || exit /b 1

echo Assembled shaders\iidx_vs11.cso and shaders\iidx_ps11.cso
endlocal
