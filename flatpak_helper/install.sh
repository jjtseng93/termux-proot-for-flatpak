#!/bin/sh

sd=$(dirname "$(realpath "$0")")

bwbin=$(command -v bwrap)

if head -c 4 "$bwbin" | grep -q ELF ; then
  echo Moving the original bwrap to bwrap-real
  mv "$bwbin" "$(dirname "$bwbin")"/bwrap-real
  chmod +x "$sd"/bwrap_wrapper.sh
  ln -s "$sd"/bwrap_wrapper.sh "$bwbin"
else
  echo Already installed!
fi

