# proot
=====
[![Travis build status](https://travis-ci.org/termux/proot.svg?branch=master)](https://travis-ci.org/termux/proot)

## Inherited
This is a copy of the Termux proot
which is a copy of [the PRoot project](https://github.com/proot-me/PRoot/) with patches applied to work better under [Termux](https://termux.com).

## Intro & Purpose
- To run Flatpaks in Android Termux proot
- Still in an early stage, may have bugs

## Tested Flatpak Apps
- org.gnome.Calculator
- org.gnome.TextEditor
- org.kde.konsole
- org.kde.kolourpaint
- org.kde.falkon
- (Web Browser based on QtWebEngine, a Chromium-based rendering engine)

## Usage
### 1. At the proot-distro side
- proot-distro install <distro_name>
- (you can also skip install and use an existing distro)
- pd login <distro_name>
- Install your favorite window manager
  * e.g. apt install openbox / xfwm4
- apt install dbus-x11
- Install flatpak & add flathub

```shell
flatpak remote-add --if-not-exists flathub \
https://dl.flathub.org/repo/flathub.flatpakrepo
```

- flatpak install <your_package>
  * quick tip to overcome the flatpak search screen width too narrow doesn't show package name problem
  * `flatpak search konsole | grep ''`
- Exit proot-distro

### 2. At the alternative proot side (Native Termux)
- Start termux-x11 as you will in termux desktop
- termux-x11 :0 -listen tcp -ac &
- export DISPLAY=:0
- openbox &
- sh enter_rootfs.sh <distro_name>
  * It compiles a newer version of proot
  * Auto-detects existing proot-distros
- cd /path/to/flatpak_helper
- export DISPLAY=:0 
- (or your termux-x11 startup number)
- (Connects over tcp)
- dbus-launch sh
- sh run.sh <your_package>
- .
- You can install org.gnome.Calculator for testing
- If you run run.sh without args it starts org.gnome.Platform to a shell
- If an info says .so error it's due to LD_PRELOAD
- just unset LD_PRELOAD inside the flatpak container
- Enter the flatpak container by sth like this
- sh run.sh --command=/bin/sh org.kde.Platform

## Recommended Termux-X11 settings
- Pointer > Enable tap-to-move for touchpads ON
- Keyboard > Show additional keyboard > Deactivate special keys on additional key bar after each keypress ON
