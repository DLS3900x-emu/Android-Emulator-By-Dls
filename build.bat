@echo off
rem Optional: rebuild Aetheron.exe from source with Visual Studio 2022 (or Build Tools) + CMake.
cd /d "%~dp0"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 || goto :err
cmake --build build --config Release || goto :err
copy /y build\Release\Aetheron.exe Aetheron.exe
echo Built Aetheron.exe
goto :eof
:err
echo Build failed.
exit /b 1
