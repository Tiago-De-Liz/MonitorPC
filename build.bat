@echo off
rem Compila o MonitorPC. Uso:  build.bat        (so compila)
rem                            build.bat run    (compila e abre)
setlocal
cd /d "%~dp0"

rem --- Localiza o compilador C++ do Visual Studio (Build Tools) ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [erro] Nao encontrei o compilador C++ do Visual Studio. Instale "Ferramentas de Build do Visual Studio" com a carga "Desenvolvimento para desktop com C++".
    exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist build\obj mkdir build\obj

rem --- Se o app estiver aberto, pede para fechar (o exe fica travado enquanto roda) ---
if exist build\MonitorPC.exe (
    build\MonitorPC.exe --quit
    for /l %%n in (1,1,20) do (
        2>nul ( >>build\MonitorPC.exe (call ) ) && goto :unlocked
        ping -n 1 -w 150 127.0.0.1 >nul
    )
)
:unlocked

rc /nologo /fo build\obj\app.res src\app.rc
if errorlevel 1 exit /b 1

rem /O1 /GL: otimiza para tamanho.  /MT: sem dependencia de runtime externo.
rem DELAYLOAD: DLLs usadas so em momentos raros (configuracoes, elevacao, tarefa agendada) nao sao
rem carregadas no processo que fica aberto 24/7.
cl /nologo /std:c++17 /utf-8 /W4 /O1 /GL /Gy /Gw /GR- /EHs-c- /D_HAS_EXCEPTIONS=0 /MT /DNDEBUG ^
   /Fo"build\obj\\" /Fe"build\MonitorPC.exe" ^
   src\main.cpp src\render.cpp src\sensors.cpp src\fps.cpp src\config.cpp src\tasks.cpp src\settings.cpp src\autostart.cpp src\util.cpp ^
   build\obj\app.res ^
   /link /LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MANIFEST:NO /INCREMENTAL:NO ^
   user32.lib gdi32.lib advapi32.lib shell32.lib comctl32.lib ole32.lib oleaut32.lib taskschd.lib secur32.lib delayimp.lib ^
   /DELAYLOAD:shell32.dll /DELAYLOAD:comctl32.dll /DELAYLOAD:ole32.dll /DELAYLOAD:oleaut32.dll /DELAYLOAD:secur32.dll
if errorlevel 1 exit /b 1

rem Modulo do PawnIO para temperatura da CPU (AMD): precisa ficar ao lado do exe.
if exist modules\AMDFamily17.bin copy /y modules\AMDFamily17.bin build\ >nul

echo.
echo OK: build\MonitorPC.exe
if /i "%~1"=="run" start "" build\MonitorPC.exe
endlocal
