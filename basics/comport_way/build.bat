@echo off
setlocal
REM Build makcu_mover.exe (MinGW-w64 g++ 13+, C++23) against static libmakxd-cpp.
REM Builds the mak-suite C++ SDK first if the library is missing.
REM (ASCII only: cmd.exe misreads UTF-8 batch files.)

set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "SDK=%ROOT%\mak-suite\cpp"
set "SDK_BUILD=%ROOT%\build\mak-suite"
set "SDK_LIB=%SDK_BUILD%\lib\libmakxd-cpp.a"

where g++ >nul 2>&1 || (echo [-] g++ not found in PATH, need MinGW-w64 e.g. C:\mingw64\bin & exit /b 1)

if not exist "%SDK_LIB%" (
    echo [*] libmakxd-cpp.a not found - building mak-suite C++ SDK...
    cmake -S "%SDK%" -B "%SDK_BUILD%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_STATIC_ONLY=ON -DBUILD_TESTING=OFF || exit /b 1
    cmake --build "%SDK_BUILD%" --config Release || exit /b 1
)

echo [*] Compiling makcu_mover.exe ...
g++ -std=c++23 -O2 -Wall -Wextra -D_WIN32_WINNT=0x0A00 ^
    -I "%SDK%\makxd-cpp\include" ^
    "%HERE%makcu_mover.cpp" ^
    -L "%SDK_BUILD%\lib" -lmakxd-cpp ^
    -lsetupapi -luuid -lbcrypt -lws2_32 -lwinmm ^
    -static ^
    -o "%HERE%makcu_mover.exe" || exit /b 1

echo [+] OK: %HERE%makcu_mover.exe
endlocal
