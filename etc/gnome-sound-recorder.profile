# Firejail profile for gnome-sound-recorder
# Description: simple sound recordings for GNOME
# This file is overwritten after every install/update
# Persistent local customizations
include gnome-sound-recorder.local
# Persistent global definitions
include globals.local

noblacklist ${MUSIC}
noblacklist ${HOME}/.local/share/flatpak
noblacklist ${HOME}/.local/share/Trash

include disable-common.inc
include disable-devel.inc
include disable-exec.inc
include disable-interpreters.inc
include disable-passwdmgr.inc
include disable-programs.inc
include disable-xdg.inc

include whitelist-var-common.inc

caps.drop all
net none
nodvd
nogroups
nonewprivs
noroot
notv
nou2f
novideo
protocol unix
seccomp
shell none
tracelog

disable-mnt
private-cache
private-dev
private-etc alsa,asound.conf,dconf,fonts,gtk-2.0,gtk-3.0,machine-id,openal,pango,pulse,xdg
private-tmp