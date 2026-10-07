#!/bin/ksh
# Isolated read-only VC H.264 metadata trace. Does not alter GAL or cluster.
BIN=/mnt/misc1/zhook/vc_frame_trace_20261007.qnx
RUN=/tmp/vc_frame_trace_boot.qnx
PID=/tmp/vc_frame_trace_boot.pid
LOG=/mnt/misc1/zhook/vc_frame_trace.log

[[ -s "$BIN" ]] || exit 0
if [[ -s "$PID" ]]; then
    read old_pid < "$PID"
    if kill -0 "$old_pid" >/dev/null 2>&1; then
        exit 0
    fi
fi
cp "$BIN" "$RUN" || exit 0
chmod 755 "$RUN" || exit 0
trap '' HUP
"$RUN" --log "$LOG" </dev/null >/dev/null 2>&1 &
printf '%s\n' $! > "$PID"
