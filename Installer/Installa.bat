@echo off
setlocal EnableExtensions DisableDelayedExpansion
title ValST MS EQ - Installazione VST3

set "PLUGIN=ValST MS EQ.vst3"
set "SRC=%~dp0%PLUGIN%"
set "DEST_ROOT=%CommonProgramW6432%\VST3"
if not defined CommonProgramW6432 set "DEST_ROOT=%CommonProgramFiles%\VST3"
set "SILENT="

rem Uso normale: doppio clic, nessun argomento.
rem "/elevated"       = il file si e' rilanciato da solo con i permessi di amministratore.
rem un percorso       = cartella di destinazione alternativa (per prove, senza amministratore).
if /i "%~1"=="/elevated" goto :checkadmin
if not "%~1"=="" (
    set "DEST_ROOT=%~1"
    set "SILENT=1"
    goto :install
)

:checkadmin
>nul 2>&1 fltmc
if errorlevel 1 (
    if /i "%~1"=="/elevated" (
        echo.
        echo  Non sono riuscito a ottenere i permessi di amministratore.
        echo  Fai clic con il tasto destro su Installa.bat e scegli "Esegui come amministratore".
        echo.
        pause
        exit /b 1
    )
    echo.
    echo  Per installare nella cartella VST3 di sistema servono i permessi di amministratore.
    echo  Conferma la richiesta di Windows che sta per comparire...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath \"%~f0\" -ArgumentList '/elevated' -Verb RunAs"
    exit /b
)

:install
if not exist "%SRC%\Contents\x86_64-win\" (
    echo.
    echo  ERRORE: non trovo la cartella "%PLUGIN%" accanto a questo file.
    echo  Se hai scaricato uno zip, estrai TUTTO il contenuto prima di lanciare l'installazione.
    echo.
    if not defined SILENT pause
    exit /b 1
)

echo.
echo  Installazione di ValST MS EQ in:
echo      %DEST_ROOT%
echo.

if not exist "%DEST_ROOT%" mkdir "%DEST_ROOT%"

if exist "%DEST_ROOT%\%PLUGIN%" (
    rmdir /s /q "%DEST_ROOT%\%PLUGIN%" 2>nul
)
if exist "%DEST_ROOT%\%PLUGIN%" (
    echo  ERRORE: la versione precedente del plugin e' in uso.
    echo  Chiudi il DAW e lancia di nuovo l'installazione.
    echo.
    if not defined SILENT pause
    exit /b 1
)

xcopy "%SRC%" "%DEST_ROOT%\%PLUGIN%\" /E /I /Y /Q /H /R >nul
if errorlevel 1 (
    echo  ERRORE durante la copia dei file.
    echo.
    if not defined SILENT pause
    exit /b 1
)

echo  Fatto! Il plugin e' installato.
echo.
echo  Apri il tuo DAW: ValST MS EQ compare tra gli effetti VST3.
echo  Se non lo vedi, fai una nuova scansione dei plugin VST3 nelle impostazioni del DAW.
echo.
if not defined SILENT pause
exit /b 0
