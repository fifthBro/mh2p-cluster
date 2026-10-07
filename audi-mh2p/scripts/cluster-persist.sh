#!/bin/ksh

if [[ -x "/eso/bin/apps/cluster/cluster" ]]; then
        /eso/bin/apps/cluster/cluster daemon verbose=2 skip_boot_splash=1 &
fi
