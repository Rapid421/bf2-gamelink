@echo off
REM Wrapper so gl.ps1 can be run without fighting the execution policy.
REM   gl pickrender
REM   gl boundslayout
REM   gl "pushbox clear"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gl.ps1" %*
