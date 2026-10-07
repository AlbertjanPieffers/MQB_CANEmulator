@echo off
setlocal
cd /d "%~dp0"
python mqb_log_converter.py
if errorlevel 1 (
    echo.
    echo Could not start the converter.
    echo Make sure Python 3 is installed and available in PATH.
    pause
)
