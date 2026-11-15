#!/bin/bash
# Run the Sketch Book vend app inside the extracted Fedora Core 5 rootfs (WSL, as root).
# Usage: sudo bash run_vend.sh [timeout_seconds]   (0 = no timeout)
set -u
F=${ROOTFS:?set ROOTFS to the extracted Fedora Core 5 rootfs}
T=${1:-0}

mountpoint -q $F/proc || mount -t proc proc $F/proc
mountpoint -q $F/dev/pts || { mkdir -p $F/dev/pts; mount -t devpts devpts $F/dev/pts; }
mkdir -p $F/tmp/.X11-unix
mountpoint -q $F/tmp/.X11-unix || mount --bind /tmp/.X11-unix $F/tmp/.X11-unix

# minimal /dev: no /dev/mem, no real hardware
for n in "null c 1 3" "zero c 1 5" "random c 1 8" "urandom c 1 9" "tty c 5 0"; do
  set -- $n; [ -e $F/dev/$1 ] || mknod -m 666 $F/dev/$1 $2 $3 $4
done

# parallel-port dongle emulator (dongle/dongle.c), state in /vend/dongle.img, log in /vend/tmp/dongle.log
[ -f $F/lib/dongle.so ] || cp "$(dirname "$0")/dongle/dongle.so" $F/lib/dongle.so
# -vt1: plain X11/XImage video driver (vsystem.cfg vidtype=2 is DGA + /dev/mem framebuffer, can't work here)
CMD="cd /vend && LD_PRELOAD=/lib/dongle.so exec /vend/bin/bttv/v8mnu22b ${VEND_ARGS--vt1}"
if [ "$T" != 0 ]; then
  exec timeout -s KILL "$T" chroot $F /usr/bin/env -i DISPLAY=${VEND_DISPLAY:-:0} HOME=/vend TMPDIR=/vend/tmp \
    PATH=.:/vend/bin:/bin:/usr/bin:/usr/X11R6/bin /bin/sh -c "$CMD"
else
  exec chroot $F /usr/bin/env -i DISPLAY=${VEND_DISPLAY:-:0} HOME=/vend TMPDIR=/vend/tmp \
    PATH=.:/vend/bin:/bin:/usr/bin:/usr/X11R6/bin /bin/sh -c "$CMD"
fi
