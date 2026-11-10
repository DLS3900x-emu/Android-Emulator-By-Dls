@echo off
rem Starts Aetheron from this folder. Logs: serial.log, hvc*.log, trace.log
cd /d "%~dp0"
start "" "%~dp0Aetheron.exe"
