#!/bin/bash
source "$(dirname "export RECORDER_102_PASSWORD=""")/load_env.sh"
export SSH_ASKPASS="D:/MeetingRecord/recorder-core/scripts/askpass.cmd"
export DISPLAY="dummy:0"

ssh -o StrictHostKeyChecking=no root@<recorder_host> "cat /opt/recorder/logs/h323trace.log" > "D:/MeetingRecord/recorder-core/logs/remote_h323trace.log"
