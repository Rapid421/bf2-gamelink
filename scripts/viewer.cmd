@echo off
REM Launch straight into a level. Must be run from an ELEVATED prompt: the game
REM re-execs itself once and the replacement process has to be injected.
REM   viewer                  -> Naboo, Blast
REM   viewer -Mode SkirmishBlast
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0viewer.ps1" -Mode Blast %*
