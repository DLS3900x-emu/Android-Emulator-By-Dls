# v4

Fixes the boot hang seen in the v3 logs: `system_server` waited forever for
the OEM-lock HAL (`android.hardware.oemlock.IOemLock/default`), was killed by
the watchdog and restarted in a loop. The HAL needs `/dev/hvc10` and a host
service answering on it; neither existed.

- **20 virtio consoles (hvc0-hvc19)** in Cuttlefish's layout instead of 4, so
  the OEM-lock, Bluetooth, NFC, UWB, GNSS... HALs find their ports and stop
  crash-looping (each crash also cost a crash dump on the single vCPU).
- **OEM-lock service on hvc10** (`src/host/oemlock.*`): implements the
  `secure_env` wire protocol; state is kept in `<cache_dir>\oemlock_state.bin`.
- **Bootconfig**: Thread node id (the Thread HAL aborted without it), audio
  HAL without a sound card, Wi-Fi settings, relaxed HAL timeouts, and the
  other values Cuttlefish's launcher passes.
- **Restored code that was in the v3 exe but missing from the v3 source**:
  virtio-vsock device, `disable-verification` for vbmeta (the shipped disk
  config did not even parse without it), automatic `androidboot.fstab_suffix`
  from the ramdisk, guest reset detection, clean shutdown, factory reset.
- **Closing the window** now asks Android to `sync` and power off first
  (close again to skip the wait). Powering off inside Android stops the VM.
- **Aetheron menu → Factory reset** erases Android data on the next start.
- **Boot stage in the title** now advances past "second stage" (the zygote
  marker never matched) and reports "boot completed" from logcat.
- Builds with MinGW-w64 on Linux again (`cmake/mingw-w64-x86_64.cmake`).

Tested here: Linux harness (new checks for the OEM-lock protocol, vsock,
vbmeta, ramdisk/fstab detection) and a clean Windows cross-build. **Not
tested: an actual boot** - that needs your Windows PC. If it stops again,
use *Collect logs* and send the zip.

Still not done: multiple vCPUs, host GPU acceleration, audio, ACPI.

# v3

- **Internet**: virtio-net card + built-in user-mode NAT (ARP, DHCP, DNS via
  the host resolver, TCP with flow control and retransmission, UDP, ping to
  the gateway). Tested here against real servers, including a 1 MiB HTTP
  download with a deliberately dropped packet.
- **Serial console window** (live ttyS0 output + command input into the
  guest).
- **Boot stage in the title bar**, detected from actual kernel/init log
  lines (no timers).
- **Aetheron menu**: collect logs into a zip, open the folder, show console.
- **Shared PCI interrupt lines** (wired-OR), so more devices can be added.
- Network can be turned off with `network = off` in `aetheron.ini`.

Not done yet (needs a first real run on your PC first): multiple vCPUs,
audio, GPU acceleration.

# What changed from the old Aetheron

## Removed: placebos and dead code

| Old | Why it was removed |
|---|---|
| Animated boot splash ("INITIALIZING WHPX…", "STARTING ANDROID 16 SERVICES & SELINUX…", progress bar) | Ran on a 2.8 s timer, not connected to anything |
| Window title "Native Hypervisor Running - Android 16" | Set unconditionally; now the title shows the real VM state |
| "Teleport" ring buffer, Teleport BAR at 0xE0000000, `aetheron_graphics_dispatch` | No guest driver ever used it; EGL/GLES handlers returned "success" and did nothing |
| `InputDispatcher` (keyboard/mouse → Teleport ring) | Nothing in the guest read it; replaced by real virtio-input devices |
| `aetheron_vhd_device` | Attached the Android disk **to the Windows host** (permanently); the guest never saw it |
| `aetheron_virtio_block.cpp` | Was not virtio, just an unused file reader |
| `aetheron_fast_boot.h`, `aetheron_efi_boot.h` | Unused ("1-second Android boot") |
| Fake PIT (counter decremented by 64 per read) | Replaced by a real 8254 driven by the host clock |
| Fake RTC (hardcoded date) | Replaced by a CMOS RTC reading real host time |
| Hardcoded MSR replies (APIC base, MISC_ENABLE, TSC) | MSRs are now handled by the hypervisor itself |
| Legacy virtio-pci bus that was never instantiated, plus `pci=off` | Replaced by a working virtio 1.x PCI transport that is actually wired in |
| "Linear framebuffer" at 0xD0000000 | Was ordinary RAM (RAM was mapped over the whole 4 GiB range, including the APIC/IOAPIC area) |
| Kernel cmdline crutches (`pci=off idle=poll lpj=… tsc=reliable no_timer_check clocksource=tsc efi=noruntime SRC= DATA=`) | They papered over the missing timer and interrupt hardware |
| Android-x86 9.0 userspace (`initrd_fast.img`, `create_fast_initrd.py`, `build_true_android16_vhd.py`, …) | That was Android 9 (2020) crammed into a 2.2 GB initrd, not Android 16 |
| Restart-Windows prompt that ran `shutdown /r` | An emulator must not reboot your PC; it now just explains what to enable |

## Added

- Correct guest memory map (3 GiB low RAM, MMIO hole, high RAM at 4 GiB)
- IOAPIC, 8254 PIT, CMOS RTC, 16550 UART, 8259 PIC, MP table
- MMIO/port-I/O instruction emulation via WinHvEmulation (handles string and
  read-modify-write instructions correctly)
- PCI bus + virtio 1.x transport; virtio-blk, virtio-console ×4,
  virtio-gpu 2D, virtio-input (touchscreen + keyboard)
- Android boot image v3/v4 parsing, vendor ramdisk table, bootconfig
- GPT composite disk from the Cuttlefish partition images, sparse image
  expansion
- DX12 display that shows only the guest's real scanout
- Settings file (`aetheron.ini`), disk layout file, bootconfig file
- Linux test harness for the host-independent parts
