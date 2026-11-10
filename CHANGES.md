# v13

- **Fixed "home screen install failed: cmd: Can't find service: package"**:
  Aetheron thought Android had finished booting about two seconds after the
  framework started (it trusted a log line that appears early when the boot
  animation is off). It now asks Android itself and waits until the package
  service answers before installing the home screen or running the clean-up
  script. The boot screen also stays up until then.
- The clean-up script runs once more on every installation, because v12
  could run it too early and still mark it as done.

# v12

- **Aetheron home screen**: Aetheron's own launcher (`apps\AetheronLauncher.apk`,
  source in `launcher/`) is installed into Android after boot and made the
  default home: dark background, clock, a search box and one grid with all
  apps. Tap to open; long-press for App info / Uninstall. Switch it off with
  `custom_launcher = off` (Settings -> Android), which also removes it.
- **Built-in app install**: Aetheron now speaks Android's debug protocol
  (adb) itself, over the virtual machine's vsock link. No `adb.exe`, no
  download. Sidebar button *Install an app*, or drop files on the window:
  `.apk` files are installed, other files go to Android's Download folder.
- The clean-up script no longer switches off apps you installed yourself.

Tested here: the adb client against a scripted adbd (harness), the launcher
compiles and passes `apksigner verify`, clean Windows cross-build. **Not
tested: the launcher running in Android, and installs on a real PC.** If the
home screen misbehaves, set `custom_launcher = off` and restart Aetheron.

# v11

- **Game controls card**: the keyboard button in the sidebar opens a small
  card with key mapping on/off, "show keys on screen", an opacity slider for
  those hints, and the way into the controls editor.
- **Controls editor**: a panel next to the Android screen. Drag a control
  onto the screen (or click it to put it in the middle), then press its key.
  On the screen, drag a control to move it, click it to change its key,
  right-click to remove it. Everything is saved as you go.
- **Control types** (all of them send real touches): tap spot, repeated tap
  (taps for as long as the key is held), W A S D joystick, swipe (drag the
  small dot to aim it).
- Key mapping is no longer a page in Settings.

Tested here: key-mapping logic incl. repeated taps and swipes (harness), the
card and the editor rendered under Wine, clean Windows cross-build. Not
tested: the popup window, drag and drop and the controls on a real PC.

# v10

- **Icons are drawn by Aetheron itself** (vector shapes) instead of coming
  from a Windows icon font, which showed as empty boxes on some PCs.

# v9

- **Key mapping** (keyboard icon in the sidebar, or Settings -> Controls):
  - *Add key*: click a spot on the Android screen, then press a key. Holding
    that key holds a finger on that spot.
  - *W A S D joystick*: place it on the game's on-screen stick; the four keys
    drag a finger around it (diagonals work).
  - Each held key is its own finger, so moving and pressing buttons work at
    the same time. The virtual touch screen is now multi-touch (10 fingers).
  - Optional hints show the keys on the Android screen. Mappings are kept in
    `config\aetheron_keymap.txt`.
- **Modern look**: rounded hover states, rounded controls on a settings card,
  pill toggles that slide.
- **Removed for now**: the install-app button, drag and drop, and the adb
  listener (it can be switched on with `adb_bridge = on` in `aetheron.ini`).

Tested here: key-mapping logic (new harness test), the chrome rendered under
Wine (`tests/ui_preview`), clean Windows cross-build. Not tested: key mapping
inside a real game on Windows.

# v8

- **Multi-core (experimental)**: Android can now get up to 8 CPU cores
  (`cpus` in `aetheron.ini`, or Settings -> Performance; default 4). The
  extra cores are started the way a PC does it (INIT / start-up IPI). If
  Android no longer boots, or hangs, set it back to 1.
- **Windows <-> Android channel (adb)**: Aetheron listens on
  `127.0.0.1:6520` and carries connections over virtio-vsock to adbd inside
  Android, like Cuttlefish does. `adb connect 127.0.0.1:6520` works with any
  adb.
- **Install apps / copy files**: new sidebar button, or drop files on the
  window. `.apk` files are installed, anything else is copied to Android's
  Download folder. Needs `adb.exe` in the `tools` folder (see
  `tools\README.txt`).
- `docs/custom-android-image.md`: how to build your own Android image
  (launcher, wallpaper, boot animation, app set) and what applies to Google
  Play.

Tested here: the vsock stream logic (new harness test) and a clean Windows
cross-build. Not tested: multi-core and adb on a real Windows PC.

Still not done: GPU acceleration (Android still draws in software), audio.

# v7

- **Own window chrome in a gray theme** (no Windows title bar or menu bar):
  - Top bar: name, Back / Home / Recent apps, the current status (or what the
    button under the mouse does), menu, minimise, maximise, close. Drag the
    bar to move the window; double-click it to maximise.
  - Sidebar: full screen (F11), screenshot (saved as .bmp in `screenshots\`),
    Android console, collect logs, open folder, and at the bottom Settings,
    Back, Home, Recent apps.
- **Settings page** (gear in the sidebar). Every control changes a real
  value in `aetheron.ini`: memory, resolution, pixel density, boot screen,
  Internet, clean-up script, factory reset. They apply on the next start.
  CPU cores and graphics are shown as plain facts because there is nothing
  to choose yet (1 core, software rendering).
- Pixel density moved to `display_dpi` in `aetheron.ini`.
- The serial console window no longer opens by itself at start.
- The display thread no longer copies and presents frames when Android has
  drawn nothing new.

Not tested on Windows (cross-compiled only).

# v5

- **Screen is no longer stretched.** The Android screen keeps its aspect
  ratio and is centred with black bars when the window is wide or maximised;
  touches still land where you click.
- **Clean boot screen**: Aetheron logo, progress bar and the real boot stage
  until Android is ready (`boot_splash` in `aetheron.ini`; *Aetheron menu →
  Show Android screen now* skips it). Android's own boot animation is turned
  off, which also saves CPU during boot.
- **Clean home screen**: after the first boot, `config\aetheron_setup.sh` is
  typed into Android's console once. It switches off every app with an icon
  except Settings, the Play Store and the file picker, enables the dark
  theme, shortens animations and removes the lock screen. Edit the file to
  change what stays (`first_boot_setup` in `aetheron.ini` turns it off).

Not tested on Windows: the boot screen drawing and the setup script. The
Play Store is **not** part of AOSP Cuttlefish images; it only appears if the
Android image itself contains Google apps.

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
