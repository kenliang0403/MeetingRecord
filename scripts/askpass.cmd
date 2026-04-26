@echo off
if defined RECORDER_102_PASSWORD (
  echo %RECORDER_102_PASSWORD%
  exit /b 0
)
exit /b 1
