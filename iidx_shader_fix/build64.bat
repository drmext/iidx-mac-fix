@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d "%~dp0"

python shaders\gen_bytecode.py || exit /b 1

if not exist obj64 mkdir obj64

cl /nologo /O2 /W3 /MT /EHs- /GS- /I ..\minhook\include /I . ^
  /Foobj64\ /Fdobj64\iidx_shader_fix_64bit.pdb ^
  iidx_shader_fix.cpp ^
  ..\minhook\src\buffer.c ..\minhook\src\hook.c ^
  ..\minhook\src\trampoline.c ..\minhook\src\hde\hde64.c ^
  /link /DLL /MACHINE:X64 /OUT:iidx_shader_fix_64bit.dll /PDB:iidx_shader_fix_64bit.pdb ^
  user32.lib /OPT:REF /OPT:ICF /INCREMENTAL:NO || exit /b 1

echo Built iidx_shader_fix_64bit.dll
endlocal
