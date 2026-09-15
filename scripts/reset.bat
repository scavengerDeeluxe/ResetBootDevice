@echo off
setlocal EnableDelayedExpansion

set "URL=https://github.com/scavengerDeeluxe/ResetBootDevice/raw/refs/heads/master/scripts/ResetBootDevice.zip"
set "WORKDIR=X:\ResetBootDevice"
set "ZIPFILE=%WORKDIR%\ResetBootDevice.zip"
set "EXTRACT=%WORKDIR%\Extracted"

:: ── Setup ──────────────────────────────────────────────────────────────────
echo [*] Preparing work directory...
if exist "%WORKDIR%" rmdir /s /q "%WORKDIR%"
mkdir "%WORKDIR%"
mkdir "%EXTRACT%"

:: ── Download ───────────────────────────────────────────────────────────────
echo [*] Downloading ResetBootDevice.zip...
certutil -urlcache -split -f "%URL%" "%ZIPFILE%" >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [!] certutil failed, trying curl...
    curl -L "%URL%" -o "%ZIPFILE%"
    if %ERRORLEVEL% neq 0 (
        echo [ERROR] Download failed. Check network connectivity.
        exit /b 1
    )
)
echo [+] Download complete.

:: ── Extract ────────────────────────────────────────────────────────────────
echo [*] Extracting archive...
tar -xf "%ZIPFILE%" -C "%EXTRACT%"
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Extraction failed. tar may not be available in this WinRE build.
    echo         Try booting to the installed OS and running from there.
    exit /b 1
)
echo [+] Extraction complete.

:: ── Locate EXE ─────────────────────────────────────────────────────────────
echo [*] Locating ResetBootDevice.exe...
set "EXE="
for /r "%EXTRACT%" %%F in (ResetBootDevice.exe) do (
    if not defined EXE set "EXE=%%F"
)

if not defined EXE (
    echo [ERROR] ResetBootDevice.exe not found in extracted archive.
    exit /b 1
)
echo [+] Found: %EXE%

:: ── Run ────────────────────────────────────────────────────────────────────
for %%F in ("%EXE%") do set "EXEDIR=%%~dpF"
echo [*] Running from: %EXEDIR%
pushd "%EXEDIR%"
"%EXE%"
set "EXITCODE=%ERRORLEVEL%"
popd

echo [*] ResetBootDevice.exe exited with code %EXITCODE%
exit /b %EXITCODE%
