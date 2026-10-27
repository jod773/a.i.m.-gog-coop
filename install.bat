@echo off
rem Double-click: installs the latest release into both games (folders from install.local.ini),
rem keeping your mod settings. Arguments are passed on, see tools\install.py.
cd /d "%~dp0"
git pull --quiet
python tools\install.py %*
pause
