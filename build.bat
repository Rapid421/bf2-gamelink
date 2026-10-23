@echo off
REM Build FrostyGameLink.dll (default) or run the UI logic unit tests ("build.bat test").
REM Run from anywhere; everything is relative to this script's folder.
REM
REM vcvars64.bat must run in the same shell as cl, and it shells out to vswhere
REM itself - so this is a plain batch file rather than something PowerShell composes.

setlocal

REM Any Visual Studio 2019+ with the C++ tools: ask vswhere, fall back to 2022 Community.
set "VSPATH=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" (
    echo vcvars64.bat not found at "%VCVARS%" - install Visual Studio with the C++ workload
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo vcvars64 failed
    exit /b 1
)

REM This script's own folder, so the source builds wherever it is checked out.
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "OUT=%ROOT%\bin"
set "OBJ=%ROOT%\obj"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%\dll" mkdir "%OBJ%\dll"
if not exist "%OBJ%\test" mkdir "%OBJ%\test"

REM UI logic with no game or ImGui dependency - unit-tested on its own.
set UI_LOGIC="%ROOT%\src\ui\command_queue.cpp" "%ROOT%\src\ui\settings.cpp" "%ROOT%\src\ui\actions.cpp" "%ROOT%\src\ui\menu_state.cpp" "%ROOT%\src\ui\history.cpp"

if /i "%~1"=="test" goto :test

REM /EHa: the structured-exception guards around engine memory need it.
REM src\imgui: Dear ImGui 1.78 (MIT, LICENSE.txt there) for the in-game overlay and menu - copied from the
REM pinned OpenGameCamera repo, backends' include paths flattened. No gamepad: no XInput dependency.
cl /nologo /LD /O2 /MT /EHa /W3 /std:c++17 ^
   /DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD ^
   /I"%ROOT%\src\imgui" /I"%ROOT%\src" ^
   /Fe:"%OUT%\FrostyGameLink.dll" ^
   /Fo:"%OBJ%\dll\\" ^
   "%ROOT%\src\main.cpp" %UI_LOGIC% "%ROOT%\src\ui\menu.cpp" "%ROOT%\src\ui\style.cpp" ^
   "%ROOT%\src\ui\tab_diagnostics.cpp" "%ROOT%\src\ui\tab_advanced.cpp" ^
   "%ROOT%\src\imgui\imgui.cpp" "%ROOT%\src\imgui\imgui_draw.cpp" "%ROOT%\src\imgui\imgui_widgets.cpp" ^
   "%ROOT%\src\imgui\imgui_impl_dx11.cpp" "%ROOT%\src\imgui\imgui_impl_win32.cpp" ^
   /link /DLL ws2_32.lib psapi.lib dbghelp.lib user32.lib d3dcompiler.lib gdi32.lib
exit /b %errorlevel%

:test
cl /nologo /O2 /MT /EHsc /W3 /std:c++17 /I"%ROOT%\src" ^
   /Fe:"%OUT%\ui_tests.exe" /Fo:"%OBJ%\test\\" ^
   "%ROOT%\tests\*.cpp" %UI_LOGIC% ^
   /link user32.lib shell32.lib
if errorlevel 1 exit /b 1
"%OUT%\ui_tests.exe"
exit /b %errorlevel%
