proot
=====
[![Travis build status](https://travis-ci.org/termux/proot.svg?branch=master)](https://travis-ci.org/termux/proot)

## Inherited
This is a copy of the Termux proot
which is a copy of [the PRoot project](https://github.com/proot-me/PRoot/) with patches applied to work better under [Termux](https://termux.com).

## Intro & Purpose
- To run flatpaks in Android Termux proot
- Still in an early stage, may have bugs

## Usage
- proot-distro install <distro_name>
- pd login <distro_name>
- install flatpak & add flathub
- install your favorite window manager
  * e.g. openbox / xfwm4

```shell
flatpak remote-add --if-not-exists flathub \
https://dl.flathub.org/repo/flathub.flatpakrepo
```

- flatpak install <your_package>
- Exit proot-distro
- Start termux-x11 as you will in termux desktop
- termux-x11 :0 -listen tcp -ac &
- export DISPLAY=:0
- openbox &
- sh enter_rootfs.sh <distro_name>
  * It compiles a newer version of proot
- cd /path/to/flatpak_helper
- export DISPLAY=:0 
- (or your termux-x11 startup number)
- (Connects over tcp)
- sh run.sh <your_package>
- .
- You can install org.gnome.Calculator for testing
- If you run run.sh without args it starts org.gnome.Platform to a shell
- If an info says .so error it's due to LD_PRELOAD
- just unset LD_PRELOAD inside the flatpak container

## Recommended Termux-X11 settings
- Pointer > Enable tap-to-move for touchpads ON
- Keyboard > Show additional keyboard > Deactivate special keys on additional key bar after each keypress ON
