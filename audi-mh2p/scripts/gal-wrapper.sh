#!/bin/ksh
# Keep the working AA/VC preload and display role, but make the process
# supervised by smartphone_integrator be the real GAL process. Without exec,
# a forced stop of this shell can leave gal.real alive and block the next run.

GAL_STABLE_PRELOAD="/mnt/app/eso/bin/apps/cluster/gal_cluster.so"
GAL_ROTARY_PRELOAD="/mnt/app/eso/bin/apps/cluster/gal_cluster_rotary_nohook.so"
GAL_PRELOAD="$GAL_STABLE_PRELOAD:$GAL_ROTARY_PRELOAD"
GAL_PRELOAD_LOG="/tmp/gal_preload.log"
export GAL_CLUSTER_DISPLAY_TYPE=2

echo -e "\ngal_date: $(date)" >> $GAL_PRELOAD_LOG
if [ -f $GAL_STABLE_PRELOAD ] && [ -f $GAL_ROTARY_PRELOAD ]; then
    export LD_PRELOAD=$GAL_PRELOAD
    echo "gal preload: $GAL_PRELOAD" >> $GAL_PRELOAD_LOG
else
    echo "error preload: $GAL_PRELOAD not found" >> $GAL_PRELOAD_LOG
fi

echo "gal_start: $* LD_PRELOAD: $LD_PRELOAD LD_LIBRARY_PATH:$LD_LIBRARY_PATH" >> $GAL_PRELOAD_LOG
exec /mnt/app/eso/bin/apps/gal.real "$@"
