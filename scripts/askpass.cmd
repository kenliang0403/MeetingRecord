@echo off
rem SSH_ASKPASS helper. Used by upload_*.ps1 / install_*.ps1 / deploy_*.ps1.
rem Reads from RECORDER_SSH_PASSWORD (preferred) or legacy RECORDER_102_PASSWORD.
if defined RECORDER_SSH_PASSWORD (
  echo %RECORDER_SSH_PASSWORD%
  exit /b 0
)
if defined RECORDER_102_PASSWORD (
  echo %RECORDER_102_PASSWORD%
  exit /b 0
)
exit /b 1
