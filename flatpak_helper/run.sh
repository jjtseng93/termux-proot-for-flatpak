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


# 2. Install bwrap wrapper
echo "[run.sh] Installing bwrap wrapper..."
sh "$sd"/install.sh



# 3. Ensure XDG_RUNTIME_DIR exists (flatpak needs it for instance data / /.flatpak-info)
XRT=/run/user/$(id -u)
if [ -z "$XDG_RUNTIME_DIR" ]; then
  mkdir -p "$XRT"
  chmod 700 "$XRT"
  export XDG_RUNTIME_DIR="$XRT"
fi

# 4. Ensure a D-Bus session bus is running.
#    flatpak only proxies the session bus into the sandbox when
#    DBUS_SESSION_BUS_ADDRESS is set on the host.  Without it
#    DBUS_SESSION_BUS_ADDRESS is unset inside the sandbox and
#    flatpak-spawn --host cannot reach org.freedesktop.Flatpak.
#    We cache the socket path so repeated run.sh calls reuse the same daemon.
_DBUS_ENV="/tmp/dbus-session-$(id -u).env"
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ] && [ -f "$_DBUS_ENV" ]; then
  # shellcheck disable=SC1090
  . "$_DBUS_ENV"
fi
# Check whether the socket is still alive.
_dbus_sock="${DBUS_SESSION_BUS_ADDRESS#unix:path=}"
_dbus_sock="${_dbus_sock%%,*}"
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ] || [ ! -S "$_dbus_sock" ]; then
  eval "$(dbus-launch --sh-syntax)"
  printf 'DBUS_SESSION_BUS_ADDRESS="%s"\nDBUS_SESSION_BUS_PID="%s"\n' \
         "$DBUS_SESSION_BUS_ADDRESS" "$DBUS_SESSION_BUS_PID" > "$_DBUS_ENV"
fi
export DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID

# 6. Run flatpak
if [ -z "$1" ] ; then
  export LD_PRELOAD="$SHIM"
  exec flatpak run --env=LD_LIBRARY_PATH=/app/lib:/app/lib/aarch64-linux-gnu --command=/bin/sh org.gnome.Platform
else
  export LD_PRELOAD="$SHIM"
  exec flatpak run --env=LD_LIBRARY_PATH=/app/lib:/app/lib/aarch64-linux-gnu "$@"
fi
