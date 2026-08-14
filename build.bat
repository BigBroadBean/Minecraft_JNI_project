@echo off
setlocal
set "GCC=C:\Users\11407\scoop\apps\gcc\current\bin\g++.exe"
if not exist "%GCC%" (echo [!!] g++ not found: %GCC% & exit /b 1)
echo === build MCCombatStatusJni.dll ===
"%GCC%" -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ -Wl,--retain-symbols-file=symkeep.txt -Iinclude -Iinclude\win32 -o MCCombatStatusJni.dll src\MCCombatStatusJni.cpp -lws2_32
if errorlevel 1 goto :err
echo === generate src\payload.h (XOR 加密载荷) ===
powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen_payload.ps1
if errorlevel 1 goto :err
echo === build injector.exe (手动映射) ===
"%GCC%" -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -o injector.exe src\injector.cpp -luser32
if errorlevel 1 goto :err
echo === build glfw_proxy.dll (启动自行加载, 零注入; 导出转发到 glfw_orig) ===
"%GCC%" -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ -Wl,--retain-symbols-file=symkeep.txt -Iinclude -Iinclude\win32 -o glfw_proxy.dll src\MCCombatStatusJni.cpp src\glfw_proxy.def
if errorlevel 1 goto :err
echo === build test\swapclient\native\swapstub.dll (hook 冒烟测试辅助库) ===
"%GCC%" -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ -s -Iinclude -Iinclude\win32 -o test\swapclient\native\swapstub.dll test\swapclient\native\swapstub.cpp -lgdi32
if errorlevel 1 goto :err
echo.
echo [OK] done: MCCombatStatusJni.dll + injector.exe (嵌入加密载荷, 手动映射)
exit /b 0
:err
echo [!!] build failed
exit /b 1
