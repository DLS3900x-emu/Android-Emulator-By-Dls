# Aetheron

A Windows x64 hypervisor that boots **real Android 16 (Cuttlefish x86_64)**
on the Windows Hypervisor Platform (WHPX). The kernel, ramdisks and system
partitions come straight from Google's Cuttlefish build. Nothing is faked.

## Quick start

1. Turn on **Windows Hypervisor Platform** and **Virtual Machine Platform** in
   *Turn Windows features on or off*, restart Windows, and make sure
   virtualization (Intel VT-x / AMD-V) is enabled in your BIOS/UEFI.
2. Put your Cuttlefish images in `android16_ci\images\` (see the note there),
   or point `images_dir` in `aetheron.ini` at the folder that has them.
3. **Set `cache_dir` in `aetheron.ini` to a folder outside OneDrive with
   ~16 GB free** (for example `C:\AetheronCache`). The first run expands the
   sparse `super.img` and `userdata.img` there, which takes a few minutes.
4. Double-click `run.bat` (or `Aetheron.exe`).

The window title shows the real state ("loading Android boot images",
"preparing disk", "guest running", or "guest stopped: <reason>"). The window
stays black until Android's graphics driver draws something.

### Using it

- **Aetheron menu → Show serial console**: the live kernel/Android console.
  Type commands at the bottom (for example `getprop ro.build.version.release`
  or `logcat -d | tail`) once Android's console shell is up.
- **Aetheron menu → Collect logs (zip)**: packs every log and setting into
  `aetheron_logs_<date>.zip` and shows it in Explorer. Send me that file.
- The title bar shows the boot stage, taken from real kernel/init log lines:
  *Linux kernel starting → unpacking ramdisk → Android first-stage init →
  second stage → framework (zygote) → boot completed*.

### Logs (send these when reporting a problem)

| File | Contents |
|---|---|
| `serial.log` | Guest kernel console (ttyS0): kernel boot messages, init |
| `hvc0.log` … `hvc19.log` | Guest virtio consoles `/dev/hvc0..19` (`hvc2.log` is logcat; most others stay empty) |
| `trace.log` | Host side: disk layout, PCI devices, bootconfig, kernel command line, anything unimplemented the guest touched, and why the VM stopped |

## How it boots

1. `boot.img` (kernel), `init_boot.img` (generic ramdisk) and
   `vendor_boot.img` (vendor ramdisks, vendor cmdline and bootconfig) are
   parsed (boot image header v3/v4).
2. The initrd is assembled as an Android bootloader would: vendor ramdisks,
   then the generic ramdisk, then a **bootconfig** block (vendor values merged
   with `config\aetheron_bootconfig.txt`).
3. A **GPT disk** is assembled from the partition images listed in
   `config\aetheron_disk.cfg` without copying them (sparse images are
   expanded once into the cache). It is exposed as a virtio-blk PCI device,
   and `androidboot.boot_devices` is set to its PCI address so first-stage
   init can find `/dev/block/by-name/*`.
4. The kernel is started through the Linux x86 32-bit boot protocol, with an
   e820 memory map and an Intel MP table describing the CPU, the IOAPIC and the
   ISA/PCI interrupt routing.

## Virtual hardware

| Device | Details |
|---|---|
| Memory | Low RAM 0 to 3 GiB, the rest at 4 GiB; 3-4 GiB is a real MMIO hole |
| Local APIC | Emulated inside the hypervisor (WHPX xAPIC mode) |
| IOAPIC | 0xFEC00000, 24 pins, edge/level, Remote IRR + EOI handling |
| 8254 PIT + port 0x61 | Driven by the host clock; used by Linux to calibrate the TSC and LAPIC timer |
| CMOS RTC | Real host UTC time, BCD/binary, 12/24 h, guest-settable (kept as an offset) |
| 16550A UART | COM1, IRQ4 → `serial.log` |
| 8259 PIC | Register-accurate, masked (all delivery goes through the IOAPIC) |
| PCI | Type-1 config access, host bridge, memory BAR decoding |
| virtio 1.x PCI (modern) | blk (disk), 20 × console (hvc0-hvc19, Cuttlefish layout; hvc10 has a built-in OEM-lock service), gpu (2D), input (touchscreen, keyboard), net, vsock |
| Network | Built-in user-mode NAT: DHCP (guest 10.0.2.15), DNS via the host resolver (10.0.2.3), TCP and UDP to the Internet; 10.0.2.2 reaches the host's localhost. No admin rights or drivers needed |

Mouse clicks and drags in the window are sent as touches, and the keyboard
as a Linux keyboard (Esc = Back).

## Known limits (honest list)

- **Untested on real hardware yet.** This version was written and
  cross-compiled on Linux. The disk, boot-image, MP-table, PIT, IOAPIC,
  virtio-blk, virtio-net and NAT code (including a real 1 MiB download
  with packet loss) pass the tests in `tests/linux_harness`, but the
  hypervisor only runs on Windows. The first runs will likely expose bugs:
  send `serial.log` and `trace.log`.
- **1 vCPU.** Starting secondary CPUs (INIT/SIPI) is not implemented yet.
- **No ACPI**, so Android can't be powered off from inside (close the window).
  No HPET (Linux only finds HPET through ACPI).
- **RTC alarm/periodic interrupts** are not implemented (logged if used).
- **Graphics are software-rendered** in the guest (virtio-gpu 2D +
  SwiftShader/ANGLE), so the UI will be slow. No host GPU acceleration yet.
- **Network**: IPv4 only, and no ping to Internet hosts (ping works for
  10.0.2.2). Cuttlefish normally gets Wi-Fi from host-side tools; whether
  Android 16 brings this network card up as Wi-Fi/Ethernet automatically is
  still to be confirmed on a real run.
- **No audio, camera or sensors.**
- The Android-specific values in `config\aetheron_bootconfig.txt` follow
  Cuttlefish's guest-only setup. If a service fails to start, compare them
  with `device/google/cuttlefish/host/commands/assemble_cvd/bootconfig_args.cpp`
  for your build.

## Building from source (optional)

`Aetheron.exe` is prebuilt. To rebuild it, use Visual Studio 2022 / Build
Tools with CMake: run `build.bat`. MinGW-w64 also works (that's how the
included exe was built, statically linked).
