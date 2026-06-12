@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S "D:\Mes documents\GitHub\Bourgeon" -B "D:\Mes documents\GitHub\Bourgeon\build" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
