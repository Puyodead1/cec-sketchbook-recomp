set -e
cd $(cd "$(dirname "$0")/.." && pwd)/dongle && gcc -m32 -shared -fPIC -nostdlib -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wl,--hash-style=sysv -Wl,-z,norelro -o dongle.so dongle.c
F=/home/puyodead1/sketch/rootfs
rsync -a --delete /mnt/sketch-root/vend/ $F/vend/
cp $(cd "$(dirname "$0")/.." && pwd)/dongle/dongle.so $F/lib/dongle.so
rm -f $F/vend/dongle.img $F/vend/tmp/dongle.log
ls -l $F/lib/dongle.so; ls -d /vend 2>&1 || true
