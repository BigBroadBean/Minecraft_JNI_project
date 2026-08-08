@echo off
setlocal
set "GCC=C:\Users\11407\scoop\apps\gcc\current\bin\g++.exe"
if not exist "%GCC%" (echo [!!] g++ not found: %GCC% & exit /b 1)
echo === build MCCanAttackJni.dll ===
"%GCC%" -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -Iinclude -Iinclude\win32 -o MCCanAttackJni.dll src\MCCanAttackJni.cpp
if errorlevel 1 goto :err
echo === build injector.exe ===
"%GCC%" -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -o injector.exe src\injector.cpp -luser32
if errorlevel 1 goto :err
echo.
echo [OK] done: MCCanAttackJni.dll + injector.exe
exit /b 0
:err
echo [!!] build failed
exit /b 1
