@echo off
rem QA for netlab: the conformance run, or SKIP where there is no game data
rem (the test VMs have none; see docs/testing.md).
if not exist "%~dp0..\game\GAME\MAIN.EXE" (echo SKIP: no game data in game\ & exit /b 0)
py -3 "%~dp0conformance.py" --exe "%~1"
exit /b %errorlevel%
