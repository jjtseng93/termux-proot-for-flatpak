#!/bin/sh
set -e

sd=$(dirname "$(realpath "$0")")

SHIM=$sd/libflatpak-preload.so
WRAPPER=$(command -v bwrap)


# 1. Build shim if missing or source is newer
SRC="$sd"/flatpak-preload.c

if [ ! -f "$SHIM" ] || [ "$SRC" -nt "$SHIM" ]; then
  echo "[run.sh] building shim..."
  cc -shared -fPIC -O2 -o "$SHIM" "$SRC" -ldl
fi


# 2. Install bwrap wrapper if missing
if head -c4 "$WRAPPER" | grep -q ELF ; then
  echo "[run.sh] Installing bwrap wrapper..."
  sh "$sd"/install.sh
fi


# 3. Run flatpak
if [ -z "$1" ] ; then
  export LD_PRELOAD="$SHIM" 
  exec flatpak run --env=LD_LIBRARY_PATH=/app/lib:/app/lib/aarch64-linux-gnu --command=/bin/sh org.gnome.Platform
else
  export LD_PRELOAD="$SHIM"
  exec flatpak run --env=LD_LIBRARY_PATH=/app/lib:/app/lib/aarch64-linux-gnu "$@"
fi
