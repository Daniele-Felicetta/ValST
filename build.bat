@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title ValST MS EQ - compilazione

rem Ricompila il plugin e prepara la cartella "dist" pronta da installare.
rem Requisiti: Visual Studio 2022 (o Build Tools) con "Sviluppo di applicazioni desktop con C++",
rem            CMake, git. JUCE viene scaricato automaticamente la prima volta (serve internet).

echo.
echo === Configurazione ===
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :error

echo.
echo === Compilazione (Release) ===
cmake --build build --config Release --target ValSTMSEQ_VST3 --parallel
if errorlevel 1 goto :error

set "ART=build\ValSTMSEQ_artefacts\Release\VST3\ValST MS EQ.vst3"
if not exist "%ART%\Contents\x86_64-win\" (
    echo Non trovo il plugin compilato in "%ART%"
    goto :error
)

echo.
echo === Preparazione della cartella dist ===
if exist dist rmdir /s /q dist
mkdir dist
xcopy "%ART%" "dist\ValST MS EQ.vst3\" /E /I /Y /Q /H >nul
copy /y Installer\Installa.bat    dist\ >nul
copy /y Installer\Disinstalla.bat dist\ >nul
copy /y Installer\LEGGIMI.txt     dist\ >nul
if exist LICENSE copy /y LICENSE dist\LICENSE.txt >nul

echo.
echo Fatto. Per installare: apri la cartella "dist" e fai doppio clic su Installa.bat
echo.
if /i not "%~1"=="nopause" pause
exit /b 0

:error
echo.
echo *** ERRORE: compilazione non riuscita (vedi i messaggi sopra) ***
echo.
if /i not "%~1"=="nopause" pause
exit /b 1
