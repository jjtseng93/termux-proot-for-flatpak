#!/bin/sh

sd=$(dirname "$(realpath "$0")")

export LD_PRELOAD="$sd"/libflatpak-preload.so

# flatpak makes two bwrap calls per session:
#   1. A sandbox for xdg-dbus-proxy itself
#   2. The main app sandbox
#
# In proot, bwrap cannot bind-mount the host D-Bus socket into the proxy
# sandbox, so xdg-dbus-proxy fails to connect.  Fix A: run xdg-dbus-proxy
# directly on the host for the proxy invocation.
#
# The main sandbox bwrap then gets the proxy socket path as a bind-mount
# source before the socket file exists (flatpak's sync-pipe read() returns
# early in proot).  Fix B: wait for the socket only when a proxy was actually
# started — signalled via a marker file keyed on the shared parent PID.

_running_proxy=0
_proxy_args_fd=-1
_past_sep=0
for _arg in "$@"; do
  if [ "$_past_sep" = "1" ]; then
    case "$_arg" in
      xdg-dbus-proxy) _running_proxy=1 ;;
      --args=*)        _proxy_args_fd="${_arg#--args=}" ;;
    esac
  fi
  [ "$_arg" = "--" ] && _past_sep=1
done

_proxy_marker="/tmp/fpshim-proxy-$PPID"

if [ "$_running_proxy" = "1" ] && [ "$_proxy_args_fd" -ge 0 ] 2>/dev/null; then
  # Touch marker so sibling main-sandbox wrapper knows to wait for the socket.
  touch "$_proxy_marker" 2>/dev/null
  exec xdg-dbus-proxy "--args=$_proxy_args_fd"
fi

# Main sandbox: check if a proxy was started by this flatpak session.
# Allow up to 100 ms for the proxy wrapper to create the marker.
_j=0
while [ $_j -lt 2 ] && [ ! -f "$_proxy_marker" ]; do
  sleep 0.05
  _j=$((_j + 1))
done

if [ -f "$_proxy_marker" ]; then
  rm -f "$_proxy_marker"
  _proxy_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/.dbus-proxy"
  _i=0
  while [ $_i -lt 100 ]; do
    if [ -d "$_proxy_dir" ]; then
      for _f in "$_proxy_dir"/*; do
        [ -S "$_f" ] && break 2
      done
    fi
    sleep 0.05
    _i=$((_i + 1))
  done
fi

exec bwrap-real --bind /dev/ptmx /dev/pts/ptmx --bind /data /data "$@"
