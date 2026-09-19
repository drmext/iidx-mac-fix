@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"

if not exist obj64 mkdir obj64

cl /nologo /O2 /W3 /MT /EHs- /GS- /D_CRT_SECURE_NO_WARNINGS /I ..\minhook\include ^
  /Foobj64\ /Fdobj64\dmo_stub_64bit.pdb ^
  dmo_stub.cpp ^
  ..\minhook\src\buffer.c ..\minhook\src\hook.c ^
  ..\minhook\src\trampoline.c ..\minhook\src\hde\hde64.c ^
  /link /DLL /MACHINE:X64 /OUT:dmo_stub_64bit.dll /PDB:dmo_stub_64bit.pdb ^
  user32.lib ole32.lib psapi.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO || exit /b 1

echo Built dmo_stub_64bit.dll
endlocal
