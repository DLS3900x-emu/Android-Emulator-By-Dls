// Aetheron: boots an Android (Cuttlefish x86_64) guest on the Windows
// Hypervisor Platform.
//
// Files written next to the executable's working folder:
//   serial.log   - guest kernel console (ttyS0)
//   hvc0..19.log - guest virtio consoles (/dev/hvc0..19; hvc2 is logcat)
//   trace.log    - host-side VMM diagnostics
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>

#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "boot/android_boot.h"
#include "boot/linux_loader.h"
#include "boot/mptable.h"
#include "boot/ramdisk_inspect.h"
#include "common/file_util.h"
#include "common/host_clock.h"
#include "common/log.h"
#include "devices/cmos_rtc.h"
#include "devices/ioapic.h"
#include "devices/pci.h"
#include "devices/pic8259.h"
#include "devices/pit.h"
#include "devices/uart16550.h"
#include "devices/virtio_blk.h"
#include "devices/virtio_console.h"
#include "devices/virtio_gpu.h"
#include "devices/virtio_input.h"
#include "devices/virtio_net.h"
#include "devices/virtio_vsock.h"
#include "disk/composite_disk.h"
#include "disk/disk_config.h"
#include "host/oemlock.h"
#include "ui/dx12_display.h"
#include "ui/boot_stage.h"
#include "ui/keymap.h"
#include "ui/serial_console.h"
#include "vmm/guest_memory.h"
#include "vmm/whpx_vm.h"

using namespace Aetheron;

namespace {

constexpr UINT WM_APP_STATUS = WM_APP + 1;

// PCI slot assignment. The block device slot is also what Android's
// first-stage init needs in androidboot.boot_devices.
constexpr uint8_t kSlotDisk = 1;
constexpr uint8_t kSlotHvc0 = 2;  // hvc0..hvc3 in slots 2..5
// Cuttlefish's guest HALs expect the launcher's console layout: hvc0 kernel
// log, hvc1 console, hvc2 logcat, then one port per host-side service up to
// hvc19 (see crosvm_manager.cpp). A HAL whose port is missing crash-loops.
constexpr int kNumHvc = 20;
constexpr int kHvcLogcat = 2, kHvcOemLock = 10;
constexpr uint8_t kSlotGpu = 6;
constexpr uint8_t kSlotTouch = 7;
constexpr uint8_t kSlotKeyboard = 8;
constexpr uint8_t kSlotNet = 9;   // shares an interrupt line with slot 1 (PCI INTx is wired-OR)
constexpr uint8_t kSlotVsock = 10;
constexpr uint8_t kSlotHvc4 = 11;  // hvc4..hvc19 in slots 11..26 (the guest numbers consoles in slot order)
constexpr UINT WM_APP_CLOSE_NOW = WM_APP + 2;
constexpr wchar_t kWipeMarker[] = L"\\wipe_data_on_next_start";
constexpr UINT kCmdCollectLogs = 101, kCmdShowConsole = 102, kCmdOpenFolder = 103, kCmdExit = 104, kCmdWipe = 105;
uint8_t SlotForHvc(int i) { return uint8_t(i < 4 ? kSlotHvc0 + i : kSlotHvc4 + (i - 4)); }
uint32_t GsiForSlot(uint8_t slot) {
    if (slot < kSlotHvc4) return 16 + (slot - 1) % 8;
    // The extra service consoles are nearly idle; keep them off the lines of
    // the disk, GPU, network and logcat so those interrupts stay cheap.
    static const uint32_t kQuietLines[] = {17, 18, 20};
    return kQuietLines[(slot - kSlotHvc4) % 3];
}

struct Settings {
    std::wstring base_dir;
    std::wstring images_dir = L"android16_ci\\images";
    std::wstring cache_dir;  // default: <images>\aetheron_cache
    std::wstring disk_config = L"config\\aetheron_disk.cfg";
    std::wstring bootconfig_file = L"config\\aetheron_bootconfig.txt";
    uint64_t ram_mb = 4096;
    uint32_t cpus = 1;
    uint32_t width = 720, height = 1280;
    bool network = true;
    std::string cmdline_extra = "console=ttyS0,115200 earlycon=uart8250,io,0x3f8,115200 8250.nr_uarts=1";
};

struct App {
    Settings s;
    HWND hwnd = nullptr;
    GuestMemory mem;
    std::unique_ptr<IoApic> ioapic;
    std::unique_ptr<Pic8259> pic;
    std::unique_ptr<Pit> pit;
    std::unique_ptr<CmosRtc> rtc;
    std::unique_ptr<Uart16550> uart;
    std::unique_ptr<PciBus> pci;
    std::vector<std::unique_ptr<VirtioPciDevice>> virtio;
    VirtioInput* touch = nullptr;
    VirtioInput* keyboard = nullptr;
    VirtioNet* net = nullptr;
    std::unique_ptr<OemLockResponder> oemlock;
    std::unique_ptr<PortDevice> reset_ports;
    std::thread shutdown_thread;
    std::atomic<bool> shutting_down{false};
    std::atomic<bool> closing{false};
    SerialConsole console;
    std::unique_ptr<BootStageTracker> stage;
    ScanoutBuffer scanout;
    Dx12Display display;
    WhpxVm vm;
    std::thread setup_thread, timer_thread;
    std::atomic<bool> quit{false};
    bool touching = false;
} g;

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::wstring Resolve(const std::wstring& base, const std::wstring& p) {
    if (p.size() > 1 && (p[1] == L':' || p[0] == L'\\')) return p;
    return base + L"\\" + p;
}

void SetStatus(const std::string& text) {
    Log("status: %s", text.c_str());
    if (g.hwnd) PostMessageW(g.hwnd, WM_APP_STATUS, 0, LPARAM(new std::wstring(Widen(text))));
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Finds the folder containing aetheron.ini: the exe folder or up to 3 parents.
std::wstring FindBaseDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir = buf;
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring d = dir;
    for (int i = 0; i < 4; ++i) {
        if (FileExists(d + L"\\aetheron.ini")) return d;
        size_t p = d.find_last_of(L"\\/");
        if (p == std::wstring::npos) break;
        d = d.substr(0, p);
    }
    return dir;
}

void LoadSettings() {
    g.s.base_dir = FindBaseDir();
    std::string ini;
    ReadTextFile(g.s.base_dir + L"\\aetheron.ini", &ini);
    std::istringstream f(ini);
    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        if (k == "images_dir") g.s.images_dir = Widen(v);
        else if (k == "cache_dir") g.s.cache_dir = Widen(v);
        else if (k == "disk_config") g.s.disk_config = Widen(v);
        else if (k == "bootconfig_file") g.s.bootconfig_file = Widen(v);
        else if (k == "ram_mb") g.s.ram_mb = std::stoull(v);
        else if (k == "cpus") g.s.cpus = std::stoul(v);
        else if (k == "display_width") g.s.width = std::stoul(v);
        else if (k == "display_height") g.s.height = std::stoul(v);
        else if (k == "cmdline_extra") g.s.cmdline_extra = v;
        else if (k == "network") g.s.network = (v == "on" || v == "1" || v == "true");
    }
    g.s.images_dir = Resolve(g.s.base_dir, g.s.images_dir);
    g.s.disk_config = Resolve(g.s.base_dir, g.s.disk_config);
    g.s.bootconfig_file = Resolve(g.s.base_dir, g.s.bootconfig_file);
    g.s.cache_dir = g.s.cache_dir.empty() ? g.s.images_dir + L"\\aetheron_cache" : Resolve(g.s.base_dir, g.s.cache_dir);
    if (g.s.cpus != 1) {
        // Application processors need INIT/SIPI start-up handling that this
        // VMM does not implement yet.
        Log("settings: cpus=%u requested; only 1 vCPU is supported, using 1", g.s.cpus);
        g.s.cpus = 1;
    }
}

// Parses "key = value" bootconfig lines into an ordered map.
void ParseBootconfig(const std::string& text, std::vector<std::pair<std::string, std::string>>* kv) {
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        if (k.empty()) continue;
        bool replaced = false;
        for (auto& e : *kv)
            if (e.first == k) {
                e.second = v;
                replaced = true;
            }
        if (!replaced) kv->emplace_back(k, v);
    }
}

void TimerThread() {
    HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!t) t = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    bool halt_seen = false;
    while (!g.quit) {
        if (!halt_seen && g.stage->halted()) {
            halt_seen = true;
            g.vm.RequestStop("Android powered off");
        }
        uint64_t now = HostNowNs();
        g.pit->Poll(now);
        uint64_t next = g.pit->NextDeadlineNs();
        uint64_t wait_ns = 10000000;  // re-check at least every 10 ms
        now = HostNowNs();
        if (next != UINT64_MAX) wait_ns = next > now ? next - now : 0;
        if (wait_ns > 10000000) wait_ns = 10000000;
        if (wait_ns < 50000) continue;
        LARGE_INTEGER due;
        due.QuadPart = -int64_t(wait_ns / 100);
        SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(t, 20);
    }
    CloseHandle(t);
}

std::string Narrow(const std::wstring& w) {
    char b[MAX_PATH * 3];
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

// The ways a PC guest resets itself. There is no firmware to restart into,
// so a reset stops the VM with the cause instead of ending in a triple fault.
class ResetPorts : public PortDevice {
public:
    uint32_t PortRead(uint16_t port, uint8_t) override { return port == 0xCF9 ? 0 : 0xFF; }
    void PortWrite(uint16_t port, uint8_t size, uint32_t value) override {
        const char* via = nullptr;
        if (port == 0x64 && (value & 0xFF) == 0xFE) via = "keyboard-controller reset";
        else if (port == 0xCF9 && size == 1 && (value & 0x04)) via = "reset control register (0xCF9)";
        else if (port == 0x92 && (value & 0x01)) via = "fast reset (port 0x92)";
        if (!via) return;
        Log("guest requested a reset via %s", via);
        g.vm.RequestStop(std::string("Android restarted or powered off (") + via +
                         ") - close and start Aetheron again");
    }
};

// Factory reset requested from the menu: drop the writable partitions so they
// are rebuilt from the images on this start.
void WipeIfRequested() {
    const std::wstring marker = g.s.base_dir + kWipeMarker;
    if (!FileExists(marker)) return;
    for (const wchar_t* name : {L"userdata", L"metadata", L"misc", L"frp"}) {
        const std::wstring f = g.s.cache_dir + L"\\" + name + L".raw";
        if (!FileExists(f)) continue;
        Log("wipe: deleting %ls (%s)", f.c_str(), DeleteFileW(f.c_str()) ? "ok" : "FAILED");
    }
    DeleteFileW(marker.c_str());
}

// Sets androidboot.fstab_suffix like Cuttlefish's launcher, unless configured.
void AddFstabSuffix(const AndroidBootFiles& boot, const std::vector<PartitionSpec>& parts,
                    std::vector<std::pair<std::string, std::string>>* bc) {
    for (auto& e : *bc)
        if (e.first == "androidboot.fstab_suffix") {
            Log("fstab: using configured androidboot.fstab_suffix = %s", e.second.c_str());
            return;
        }
    std::vector<std::string> files;
    for (const auto* rd : {&boot.vendor_ramdisk, &boot.generic_ramdisk}) {
        std::string format;
        std::vector<std::string> f = ListRamdiskFiles(*rd, &format);
        Log("fstab: ramdisk format %s, %s, %zu files", format.c_str(),
            f.empty() ? "NOT readable" : "readable", f.size());
        files.insert(files.end(), f.begin(), f.end());
    }
    for (auto& f : files)
        if (f.find("fstab.") != std::string::npos) Log("fstab: ramdisk contains %s", f.c_str());
    std::string fs;
    for (auto& p : parts)
        if (p.name == "userdata") fs = DetectFilesystem(p.file);
    Log("fstab: userdata filesystem: %s", fs.empty() ? "not formatted yet (assuming f2fs)" : fs.c_str());
    std::string suffix = ChooseFstabSuffix(files, fs);
    if (suffix.empty()) {
        Log("fstab: no fstab.* file found in the ramdisks; first-stage mount will fail. Set "
            "androidboot.fstab_suffix in aetheron_bootconfig.txt");
        return;
    }
    Log("fstab: setting androidboot.fstab_suffix = %s", suffix.c_str());
    bc->emplace_back("androidboot.fstab_suffix", suffix);
}

bool SetupAndStart() {
    std::string err;
    std::string why;
    if (!WhpxVm::IsAvailable(&why)) {
        SetStatus("cannot start: " + why);
        MessageBoxW(g.hwnd,
                    L"Windows Hypervisor Platform is not available.\n\n"
                    L"Enable it in \"Turn Windows features on or off\" (tick \"Windows Hypervisor Platform\" and "
                    L"\"Virtual Machine Platform\"), make sure virtualization (VT-x / AMD-V) is enabled in your "
                    L"BIOS/UEFI, then restart Windows yourself.",
                    L"Aetheron", MB_OK | MB_ICONWARNING);
        return false;
    }

    // 1. Android boot images
    SetStatus("loading Android boot images");
    AndroidBootFiles boot;
    if (!LoadAndroidBootImages(g.s.images_dir + L"\\boot.img", g.s.images_dir + L"\\init_boot.img",
                               g.s.images_dir + L"\\vendor_boot.img", &boot, &err)) {
        SetStatus("cannot start: " + err);
        return false;
    }

    // 2. Disk
    SetStatus("preparing disk (the first run expands sparse images and can take a few minutes)");
    CreateDirectoryW(g.s.cache_dir.c_str(), nullptr);
    WipeIfRequested();
    std::vector<PartitionSpec> parts;
    if (!PrepareDiskPartitions(g.s.disk_config, g.s.images_dir, g.s.cache_dir, &parts, &err)) {
        SetStatus("cannot start: disk: " + err);
        return false;
    }
    auto disk = std::make_unique<CompositeDisk>();
    if (!disk->Build(parts)) {
        SetStatus("cannot start: failed to build the composite disk (see trace.log)");
        return false;
    }

    // 3. Bootconfig: vendor_boot's section, plus values the launcher provides.
    std::vector<std::pair<std::string, std::string>> bc;
    ParseBootconfig(boot.vendor_bootconfig, &bc);
    char bootdev[64];
    snprintf(bootdev, sizeof(bootdev), "pci0000:00/0000:00:%02x.0", kSlotDisk);
    ParseBootconfig(std::string("androidboot.boot_devices = ") + bootdev + "\n", &bc);
    ParseBootconfig("androidboot.ddr_size = " + std::to_string(g.s.ram_mb) + "MB\n", &bc);
    {
        std::string text;
        if (!ReadTextFile(g.s.bootconfig_file, &text))
            Log("bootconfig: %ls not found, using only vendor_boot values", g.s.bootconfig_file.c_str());
        ParseBootconfig(text, &bc);
    }
    AddFstabSuffix(boot, parts, &bc);
    std::string bootconfig;
    for (auto& e : bc) bootconfig += e.first + " = " + e.second + "\n";
    Log("bootconfig (%zu entries):\n%s", bc.size(), bootconfig.c_str());
    std::vector<uint8_t> initrd = BuildInitrdWithBootconfig(boot, bootconfig);

    std::string cmdline = Trim(boot.boot_cmdline + " " + boot.vendor_cmdline + " " + g.s.cmdline_extra);
    if (cmdline.find("bootconfig") == std::string::npos) cmdline += " bootconfig";
    Log("kernel cmdline: %s", cmdline.c_str());

    // 4. Memory and platform devices
    SetStatus("creating virtual machine");
    if (!g.mem.Allocate(g.s.ram_mb << 20)) {
        SetStatus("cannot start: not enough host memory for " + std::to_string(g.s.ram_mb) + " MiB");
        return false;
    }
    if (!g.vm.Create(g.s.cpus, &g.mem)) {
        SetStatus("cannot start: hypervisor partition setup failed (see trace.log)");
        return false;
    }
    const uint8_t ioapic_id = uint8_t(g.s.cpus);
    g.ioapic = std::make_unique<IoApic>(ioapic_id, [](const ApicMessage& m) { g.vm.DeliverInterrupt(m); });
    g.pic = std::make_unique<Pic8259>();
    g.pit = std::make_unique<Pit>(g.ioapic.get());
    g.rtc = std::make_unique<CmosRtc>(g.ioapic.get());
    g.rtc->SetMemorySize(g.mem.low_size(), g.mem.high_size());
    std::wstring serial_log = g.s.base_dir + L"\\serial.log";
    char serial_path[MAX_PATH * 3];
    WideCharToMultiByte(CP_ACP, 0, serial_log.c_str(), -1, serial_path, sizeof(serial_path), nullptr, nullptr);
    g.uart = std::make_unique<Uart16550>(0x3F8, 4, g.ioapic.get(), serial_path);
    g.stage = std::make_unique<BootStageTracker>([](const std::string& st) { SetStatus("guest running: " + st); });
    g.uart->SetTxObserver([](char c) {
        g.console.Append(c);
        g.stage->Feed(c);
    });
    g.pci = std::make_unique<PciBus>(g.ioapic.get());

    g.vm.SetIoApic(g.ioapic.get());
    g.vm.SetPciBus(g.pci.get());
    g.vm.RegisterPorts(0x20, 0x21, g.pic.get());
    g.vm.RegisterPorts(0xA0, 0xA1, g.pic.get());
    g.vm.RegisterPorts(0x4D0, 0x4D1, g.pic.get());
    g.vm.RegisterPorts(0x40, 0x43, g.pit.get());
    g.vm.RegisterPorts(0x61, 0x61, g.pit.get());
    g.vm.RegisterPorts(0x70, 0x71, g.rtc.get());
    g.vm.RegisterPorts(0x3F8, 0x3FF, g.uart.get());
    g.vm.RegisterPorts(0xCF8, 0xCFF, g.pci.get());
    g.reset_ports = std::make_unique<ResetPorts>();
    g.vm.RegisterPorts(0x64, 0x64, g.reset_ports.get());
    g.vm.RegisterPorts(0x92, 0x92, g.reset_ports.get());
    g.vm.RegisterPorts(0xCF9, 0xCF9, g.reset_ports.get());

    // 5. PCI devices
    std::vector<MpPciIrq> pci_irqs;
    auto add = [&](uint8_t slot, VirtioPciDevice* d) {
        g.pci->AddDevice(slot, d, GsiForSlot(slot));
        pci_irqs.push_back({slot, uint8_t(GsiForSlot(slot))});
        g.virtio.emplace_back(d);
    };
    add(kSlotDisk, new VirtioBlk(&g.mem, std::move(disk), "AETHERON-OS"));
    g.oemlock = std::make_unique<OemLockResponder>(Narrow(g.s.cache_dir + L"\\oemlock_state.bin"));
    for (int i = 0; i < kNumHvc; ++i) {
        auto* con = new VirtioConsole(&g.mem, Narrow(g.s.base_dir + L"\\hvc" + std::to_wstring(i) + L".log"));
        if (i == kHvcLogcat) {
            con->SetOutputHandler([](const uint8_t* d, size_t n) {
                for (size_t k = 0; k < n; ++k) g.stage->Feed(char(d[k]), BootStageTracker::kLogcat);
            });
        } else if (i == kHvcOemLock) {
            con->SetOutputHandler([con](const uint8_t* d, size_t n) {
                std::vector<uint8_t> reply = g.oemlock->Feed(d, n);
                if (!reply.empty()) con->SendToGuest(std::string(reply.begin(), reply.end()));
            });
        }
        add(SlotForHvc(i), con);
    }
    add(kSlotGpu, new VirtioGpu(&g.mem, &g.scanout, g.s.width, g.s.height));
    g.touch = new VirtioInput(&g.mem, VirtioInput::Kind::Touchscreen, g.s.width, g.s.height);
    add(kSlotTouch, g.touch);
    g.keyboard = new VirtioInput(&g.mem, VirtioInput::Kind::Keyboard, 0, 0);
    add(kSlotKeyboard, g.keyboard);
    if (g.s.network) {
        const uint8_t mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        g.net = new VirtioNet(&g.mem, mac);
        add(kSlotNet, g.net);
    }
    add(kSlotVsock, new VirtioVsock(&g.mem));

    // 6. Firmware tables + kernel
    WriteMpTable(g.mem.low_host(), LinuxBootLayout::kMpTableGpa, g.s.cpus, ioapic_id, IoApic::kBase, pci_irqs);
    LinuxBootResult lb;
    if (!LoadLinux(g.mem, boot.kernel, initrd, cmdline, &lb, &err)) {
        SetStatus("cannot start: " + err);
        return false;
    }
    if (!g.vm.SetupBootCpu(lb.entry_gpa, lb.boot_params_gpa, LinuxBootLayout::kGdtGpa)) {
        SetStatus("cannot start: vCPU setup failed (see trace.log)");
        return false;
    }

    g.timer_thread = std::thread(TimerThread);
    g.vm.Start([](const std::string& reason) { SetStatus("guest stopped: " + reason); });
    if (g.net) g.net->StartNetwork();
    SetStatus("guest running: waiting for kernel output");
    return true;
}

// Copies all logs and settings into a zip next to the executable and shows it.
void CollectLogs(HWND hwnd) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring dir = g.s.base_dir + L"\\aetheron_logs_" + stamp;
    std::wstring zip = dir + L".zip";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<std::wstring> files = {L"serial.log", L"trace.log", L"aetheron.ini", L"config\\aetheron_disk.cfg",
                                       L"config\\aetheron_bootconfig.txt"};
    for (int i = 0; i < kNumHvc; ++i) files.push_back(L"hvc" + std::to_wstring(i) + L".log");
    for (const std::wstring& f : files) {
        std::wstring src = g.s.base_dir + L"\\" + f;
        std::wstring name = f;
        name = name.substr(name.find_last_of(L'\\') + 1);
        CopyFileW(src.c_str(), (dir + L"\\" + name).c_str(), FALSE);
    }
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -Command \"Compress-Archive -Force -Path '" + dir +
                       L"\\*' -DestinationPath '" + zip + L"'\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    bool ok = false;
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        ok = code == 0;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    std::wstring target = ok ? zip : dir;
    std::wstring sel = L"/select,\"" + target + L"\"";
    ShellExecuteW(hwnd, L"open", L"explorer.exe", sel.c_str(), nullptr, SW_SHOWNORMAL);
    Log("logs collected into %ls", target.c_str());
}

void SendTouch(HWND hwnd, int x, int y, bool down_change, bool down) {
    if (!g.touch) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    int gx = int(int64_t(x) * g.s.width / rc.right), gy = int(int64_t(y) * g.s.height / rc.bottom);
    gx = gx < 0 ? 0 : gx >= int(g.s.width) ? int(g.s.width) - 1 : gx;
    gy = gy < 0 ? 0 : gy >= int(g.s.height) ? int(g.s.height) - 1 : gy;
    std::vector<InputEvent> ev;
    ev.push_back({3, 0, gx});  // EV_ABS ABS_X
    ev.push_back({3, 1, gy});  // EV_ABS ABS_Y
    if (down_change) ev.push_back({1, 0x14A, down ? 1 : 0});  // BTN_TOUCH
    ev.push_back({0, 0, 0});   // SYN_REPORT
    g.touch->Send(ev.data(), ev.size());
}

// Asks Android to save its data and power off before the VM is torn down, so
// the data partition is unmounted cleanly. Closing again skips the wait.
void GracefulShutdown(HWND hwnd) {
    SetStatus("shutting Android down, saving data (close again to skip)...");
    const std::string cmd = "\nsync; reboot -p\n";
    g.uart->InjectInput(cmd.data(), cmd.size());
    Log("shutdown: sent 'sync; reboot -p' to the Android console");
    for (int i = 0; i < 200 && !g.closing && g.vm.running() && !g.stage->halted(); ++i) Sleep(100);
    Log("shutdown: %s", g.stage->halted() || !g.vm.running() ? "Android powered off cleanly"
                                                              : "no power-off seen, closing anyway");
    if (!g.closing) PostMessageW(hwnd, WM_APP_CLOSE_NOW, 0, 0);
}

void RequestWipe(HWND hwnd) {
    if (MessageBoxW(hwnd,
                    L"Erase all Android data (apps, accounts, settings) the next time Aetheron starts?\n\n"
                    L"The system images are not touched.",
                    L"Aetheron - factory reset", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
        return;
    HANDLE h = CreateFileW((g.s.base_dir + kWipeMarker).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        MessageBoxW(hwnd, L"Could not write the reset marker in the Aetheron folder.", L"Aetheron", MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(h);
    Log("wipe: requested; data will be erased on the next start");
    MessageBoxW(hwnd, L"Android data will be erased on the next start. Close Aetheron and start it again.",
                L"Aetheron", MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_STATUS: {
            auto* s = reinterpret_cast<std::wstring*>(lp);
            SetWindowTextW(hwnd, (L"Aetheron - " + *s).c_str());
            delete s;
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            g.touching = true;
            SendTouch(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true, true);
            return 0;
        case WM_MOUSEMOVE:
            if (g.touching) SendTouch(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false, true);
            return 0;
        case WM_LBUTTONUP:
            if (g.touching) {
                g.touching = false;
                ReleaseCapture();
                SendTouch(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true, false);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP: {
            if (!g.keyboard) break;
            bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            bool repeat = down && (lp & (1 << 30));
            uint16_t code = VkToLinuxKey(uint32_t(wp), (lp >> 24) & 1);
            if (!code) break;
            InputEvent ev[2] = {{1, code, down ? (repeat ? 2 : 1) : 0}, {0, 0, 0}};
            g.keyboard->Send(ev, 2);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kCmdCollectLogs: CollectLogs(hwnd); return 0;
                case kCmdShowConsole: g.console.Show(); return 0;
                case kCmdOpenFolder:
                    ShellExecuteW(hwnd, L"open", g.s.base_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case kCmdWipe: RequestWipe(hwnd); return 0;
                case kCmdExit: SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
            }
            break;
        case WM_CLOSE:
            if (g.uart && g.stage && g.vm.running() && g.stage->rank() >= BootStageTracker::kRankSecondStage &&
                !g.shutting_down.exchange(true)) {
                g.shutdown_thread = std::thread(GracefulShutdown, hwnd);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_APP_CLOSE_NOW:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g.closing = true;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    LoadSettings();
    SetCurrentDirectoryW(g.s.base_dir.c_str());
    {
        std::wstring t = g.s.base_dir + L"\\trace.log";
        char b[MAX_PATH * 3];
        WideCharToMultiByte(CP_ACP, 0, t.c_str(), -1, b, sizeof(b), nullptr, nullptr);
        LogInit(b);
    }
    Log("Aetheron starting; base folder %ls, images %ls, RAM %llu MiB, display %ux%u", g.s.base_dir.c_str(),
        g.s.images_dir.c_str(), (unsigned long long)g.s.ram_mb, g.s.width, g.s.height);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = HBRUSH(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"AetheronWindow";
    RegisterClassW(&wc);

    // Window: guest aspect ratio, ~85% of the work-area height.
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    double scale = 0.85 * (work.bottom - work.top) / g.s.height;
    if (scale > 1.0) scale = 1.0;
    RECT r = {0, 0, LONG(g.s.width * scale), LONG(g.s.height * scale)};
    HMENU menu = CreateMenu();
    HMENU logs = CreatePopupMenu();
    AppendMenuW(logs, MF_STRING, kCmdShowConsole, L"Show serial &console");
    AppendMenuW(logs, MF_STRING, kCmdCollectLogs, L"&Collect logs (zip)");
    AppendMenuW(logs, MF_STRING, kCmdOpenFolder, L"&Open Aetheron folder");
    AppendMenuW(logs, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(logs, MF_STRING, kCmdWipe, L"&Factory reset (erase Android data)...");
    AppendMenuW(logs, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(logs, MF_STRING, kCmdExit, L"E&xit");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(logs), L"&Aetheron");
    RECT r2 = r;
    AdjustWindowRect(&r2, WS_OVERLAPPEDWINDOW, TRUE);
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Aetheron - starting", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, r2.right - r2.left, r2.bottom - r2.top, nullptr, menu, inst, nullptr);
    if (!g.hwnd) return 1;
    ShowWindow(g.hwnd, SW_SHOW);
    g.console.Create(inst, [](const std::string& line) {
        if (g.uart) g.uart->InjectInput(line.data(), line.size());
    });
    g.console.Show();

    std::string err;
    if (!g.display.Initialize(g.hwnd, g.s.width, g.s.height, &err)) {
        Log("display: %s", err.c_str());
        MessageBoxA(g.hwnd, ("Direct3D 12 initialisation failed: " + err).c_str(), "Aetheron", MB_OK | MB_ICONERROR);
        return 1;
    }
    g.display.Start(&g.scanout);
    g.setup_thread = std::thread([] { SetupAndStart(); });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g.hwnd = nullptr;
    if (g.shutdown_thread.joinable()) g.shutdown_thread.join();
    if (g.setup_thread.joinable()) g.setup_thread.join();
    g.vm.Stop();
    g.quit = true;
    if (g.timer_thread.joinable()) g.timer_thread.join();
    g.display.Stop();
    Log("Aetheron exited");
    LogClose();
    _exit(0);  // device objects reference each other; skip ordered destruction
}
