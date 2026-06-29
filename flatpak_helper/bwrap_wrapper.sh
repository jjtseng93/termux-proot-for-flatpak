#!/bin/sh

sd=$(dirname "$(realpath "$0")")

export LD_PRELOAD="$sd"/libflatpak-preload.so

# flatpak passes all bwrap setup via --args FD (null-terminated binary data).
# Two separate bwrap invocations happen:
#   1. A sandbox for xdg-dbus-proxy itself
#   2. The main app sandbox
#
# In proot, bwrap cannot bind-mount the host D-Bus socket into the proxy
# sandbox, so xdg-dbus-proxy can never connect to D-Bus and never creates
# its output socket.  The main sandbox then fails with "Can't find source
# path".
#
# Fix A (proxy invocation): skip bwrap entirely and run xdg-dbus-proxy
# directly on the host.  It inherits the same fds (including the sync-pipe
# fd and --args fd) so flatpak's synchronisation protocol still works.
#
# Fix B (main sandbox invocation): wait for the proxy socket to appear in
# .dbus-proxy before handing off to bwrap-real.

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

if [ "$_running_proxy" = "1" ] && [ "$_proxy_args_fd" -ge 0 ] 2>/dev/null; then
  exec xdg-dbus-proxy "--args=$_proxy_args_fd"
fi

# Main sandbox: wait up to 5 s for the proxy socket.
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

exec bwrap-real --bind /dev/ptmx /dev/pts/ptmx --bind /data /data "$@"
