@echo off
setlocal EnableExtensions DisableDelayedExpansion
title ValST MS EQ - Disinstallazione VST3

set "PLUGIN=ValST MS EQ.vst3"
set "DEST_ROOT=%CommonProgramW6432%\VST3"
if not defined CommonProgramW6432 set "DEST_ROOT=%CommonProgramFiles%\VST3"
set "SILENT="

if /i "%~1"=="/elevated" goto :checkadmin
if not "%~1"=="" (
    set "DEST_ROOT=%~1"
    set "SILENT=1"
    goto :remove
)

:checkadmin
>nul 2>&1 fltmc
if errorlevel 1 (
    if /i "%~1"=="/elevated" (
        echo.
        echo  Non sono riuscito a ottenere i permessi di amministratore.
        echo  Fai clic con il tasto destro su Disinstalla.bat e scegli "Esegui come amministratore".
        echo.
        pause
        exit /b 1
    )
    echo.
    echo  Per rimuovere il plugin servono i permessi di amministratore.
    echo  Conferma la richiesta di Windows che sta per comparire...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath \"%~f0\" -ArgumentList '/elevated' -Verb RunAs"
    exit /b
)

:remove
echo.
if not exist "%DEST_ROOT%\%PLUGIN%" (
    echo  ValST MS EQ non risulta installato in:
    echo      %DEST_ROOT%
    echo.
    if not defined SILENT pause
    exit /b 0
)

rmdir /s /q "%DEST_ROOT%\%PLUGIN%" 2>nul
if exist "%DEST_ROOT%\%PLUGIN%" (
    echo  ERRORE: il plugin e' in uso. Chiudi il DAW e riprova.
    echo.
    if not defined SILENT pause
    exit /b 1
)

echo  ValST MS EQ e' stato rimosso da:
echo      %DEST_ROOT%
echo.
if not defined SILENT pause
exit /b 0
