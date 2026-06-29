#!/bin/sh

sd=$(dirname "$(realpath "$0")")

export LD_PRELOAD="$sd"/libflatpak-preload.so 

exec bwrap-real --bind /dev/ptmx /dev/pts/ptmx --bind /data /data "$@"
