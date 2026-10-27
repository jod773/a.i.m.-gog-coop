@echo off
rem Double-click: builds main + all open pull requests (the fixes being tested) and installs that
rem test build into both games, keeping your mod settings. Compare the printed code id with your
rem friend's. PR numbers as arguments pick only those, see tools\install.py.
cd /d "%~dp0"
git checkout --quiet main
git pull --quiet
python tools\install.py --test %*
pause
