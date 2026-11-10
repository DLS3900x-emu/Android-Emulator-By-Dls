#!/bin/sh
# Host-independent tests of the disk, boot-image, MP-table, PIT, IOAPIC and
# virtio-blk code, compiled natively on Linux with a small Win32 file-API shim.
# (The hypervisor itself can only be tested on Windows.)
set -e
cd "$(dirname "$0")"
S=../../src
python3 mkimgs.py imgs
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Ishim -I$S -include cstdint -include shim/windows.h \
  test.cpp $S/disk/*.cpp $S/boot/android_boot.cpp $S/boot/mptable.cpp $S/devices/pit.cpp $S/devices/ioapic.cpp \
  $S/devices/virtio_pci.cpp $S/devices/virtio_blk.cpp $S/devices/virtio_console.cpp $S/devices/virtio_net.cpp $S/devices/virtio_vsock.cpp $S/host/oemlock.cpp $S/host/adb_client.cpp $S/host/vsock_stream.cpp $S/ui/keymapper.cpp $S/boot/ramdisk_inspect.cpp $S/net/user_net.cpp $S/devices/pci.cpp $S/vmm/guest_memory.cpp \
  $S/common/log.cpp -o test -lpthread
rm -rf imgs/cache
ASAN_OPTIONS=detect_leaks=0 ./test
python3 verify.py

# Network stack against real local servers (HTTP + UDP echo on 127.0.0.1)
head -c 1048576 /dev/urandom > payload.bin
python3 srv.py & SRV=$!
sleep 1
g++ -std=c++17 -O1 -g -fsanitize=address -Ishim -I$S -include cstdint nettest.cpp $S/net/user_net.cpp $S/common/log.cpp -o nettest -lpthread
ASAN_OPTIONS=detect_leaks=0 ./nettest || { kill $SRV; exit 1; }
kill $SRV
