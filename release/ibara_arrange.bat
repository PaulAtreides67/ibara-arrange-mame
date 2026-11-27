@echo off
rem ibara ARRANGE in MAME (self-contained ROM set in roms\, GADGETS in artwork\, its own nvram\ and sta\).
rem The first time, it asks where your mame.exe is and remembers it in mame_path.txt beside this file
rem (delete that file to choose again).
setlocal
set "HERE=%~dp0"
set "CFG=%HERE%mame_path.txt"
set "MAMEEXE="
if exist "%CFG%" set /p MAMEEXE=<"%CFG%"
if defined MAMEEXE if not exist "%MAMEEXE%" set "MAMEEXE="
if not defined MAMEEXE call :ask
if not defined MAMEEXE goto :fail
for %%f in ("%MAMEEXE%") do set "MAMEDIR=%%~dpf"
for %%d in (nvram sta) do if not exist "%HERE%%%d" mkdir "%HERE%%%d"
start "" /d "%MAMEDIR%." "%MAMEEXE%" ibara -rompath "%HERE%roms" -artpath "%HERE%artwork" -nvram_directory "%HERE%nvram" -state_directory "%HERE%sta" -plugin layout
exit /b 0

:ask
echo Where is your mame.exe? Choose it in the window that opens.
for /f "usebackq delims=" %%p in (`powershell -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; $d.Title = 'ibara ARRANGE: where is your mame.exe?'; $d.Filter = 'MAME (*.exe)|*.exe'; if ($d.ShowDialog() -eq 'OK') { $d.FileName }"`) do set "MAMEEXE=%%p"
if defined MAMEEXE goto :check
set /p "MAMEEXE=Type the full path of mame.exe (or drag mame.exe here) and press Enter: "
if not defined MAMEEXE goto :eof
set "MAMEEXE=%MAMEEXE:"=%"
:check
if exist "%MAMEEXE%" goto :save
echo Not found: %MAMEEXE%
set "MAMEEXE="
goto :eof
:save
>"%CFG%" echo %MAMEEXE%
goto :eof

:fail
echo No mame.exe chosen. Run this file again to choose it.
pause
exit /b 1
