proot
=====
[![Travis build status](https://travis-ci.org/termux/proot.svg?branch=master)](https://travis-ci.org/termux/proot)

## Inherited
This is a copy of the Termux proot
which is a copy of [the PRoot project](https://github.com/proot-me/PRoot/) with patches applied to work better under [Termux](https://termux.com).

## Purpose
- To run flatpaks in Android Termux proot
## Usage
- proot-distro install <distro_name>
- pd login <distro_name>
- install flatpak & add flathub

```shell
flatpak remote-add --if-not-exists flathub \
https://dl.flathub.org/repo/flathub.flatpakrepo
```

- flatpak install <your_package>
- exit proot-distro
- start termux-x11 as you will in termux desktop
- termux-x11 :0 -listen tcp -ac &
- export LIBGL_ALWAYS_SOFTWARE=1
- export DISPLAY=:0
- xfwm4 &
- ./enter_rootfs.sh <distro_name>
  * it compiles a newer version proot
- su <user_name>
- cd /path/to/flatpak_helper
- export DISPLAY=:0 
- (or your termux-x11 startup number)
- sh run.sh <your_package>
