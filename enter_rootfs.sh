#!/bin/sh

sd=$(dirname "$(realpath "$0")")

if [ -z "$1" ] ; then
  echo Usage: $(basename "$0") '<distro_name>'
  exit 1
fi

old_pddir=$PREFIX/var/lib/proot-distro/installed-rootfs/$1

new_pddir=$PREFIX/var/lib/proot-distro/containers/$1/rootfs

if [ -d "$new_pddir" ] ; then
  flag_rootfs=new
  rootfs=$new_pddir
elif [ -d "$old_pddir" ] ; then
  flag_rootfs=old
  rootfs=$old_pddir
else
  echo Rootfs not found: $1
  exit 1
fi


if [ -f "$PREFIX"/../applib/libproot-loader.so ] ; then
  export PROOT_LOADER="$PREFIX"/../applib/libproot-loader.so
fi


proot_bin=$sd/src/proot

if ! [ -f "$proot_bin" ] ; then
  make -C "$sd"/src
  if ! [ -f "$proot_bin" ] ; then
    echo Failed to compile proot binary
    exit 127
  fi
fi


unset LD_PRELOAD

echo ''
echo '===='
echo Rootfs from $flag_rootfs proot-distro
echo '===='

exec "$proot_bin" -S "$rootfs" \
  -b "$sd"/fakeid.txt:/proc/sys/kernel/overflowuid \
  -b "$sd"/fakeid.txt:/proc/sys/kernel/overflowgid \
  -b "$PREFIX"/tmp:/tmp \
  /bin/sh -l
