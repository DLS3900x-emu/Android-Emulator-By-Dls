// Windows Hypervisor Platform virtual machine: partition, memory mapping,
// vCPU threads, and dispatch of I/O-port and MMIO exits to emulated devices
// (instruction decoding is done by WinHvEmulation).
#pragma once
#include <windows.h>
#include <winhvemulation.h>
#include <winhvplatform.h>

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../devices/device.h"
#include "../devices/ioapic.h"
#include "../devices/pci.h"
#include "guest_memory.h"

namespace Aetheron {

class WhpxVm {
public:
    ~WhpxVm();

    static bool IsAvailable(std::string* why);

    bool Create(uint32_t cpu_count, GuestMemory* mem);
    void RegisterPorts(uint16_t first, uint16_t last, PortDevice* dev);
    void SetIoApic(IoApic* ioapic) { ioapic_ = ioapic; }
    void SetPciBus(PciBus* bus) { pci_ = bus; }

    // Puts vCPU 0 at the Linux 32-bit protected-mode entry point.
    bool SetupBootCpu(uint64_t entry_gpa, uint64_t boot_params_gpa, uint64_t gdt_gpa);

    // Delivers an interrupt message from the IOAPIC to the local APIC(s).
    void DeliverInterrupt(const ApicMessage& m);

    bool Start(std::function<void(const std::string&)> on_stop);
    void Stop();
    // Asks the vCPU loop to stop with `reason` (callable from device code
    // running on a vCPU thread, e.g. a guest-initiated reset).
    void RequestStop(const std::string& reason);
    bool running() const { return running_; }

private:
    struct VpContext {
        WhpxVm* vm;
        uint32_t index;
    };
    void VcpuThread(uint32_t index);
    bool HandleExit(uint32_t index, const WHV_RUN_VP_EXIT_CONTEXT& ctx, std::string* stop_reason);
    void HandleCpuid(uint32_t index, const WHV_RUN_VP_EXIT_CONTEXT& ctx);

    // WinHvEmulation callbacks
    static HRESULT CALLBACK EmuIoPort(void* ctx, WHV_EMULATOR_IO_ACCESS_INFO* io);
    static HRESULT CALLBACK EmuMemory(void* ctx, WHV_EMULATOR_MEMORY_ACCESS_INFO* m);
    static HRESULT CALLBACK EmuGetRegs(void* ctx, const WHV_REGISTER_NAME* names, UINT32 count,
                                       WHV_REGISTER_VALUE* values);
    static HRESULT CALLBACK EmuSetRegs(void* ctx, const WHV_REGISTER_NAME* names, UINT32 count,
                                       const WHV_REGISTER_VALUE* values);
    static HRESULT CALLBACK EmuTranslateGva(void* ctx, WHV_GUEST_VIRTUAL_ADDRESS gva, WHV_TRANSLATE_GVA_FLAGS flags,
                                            WHV_TRANSLATE_GVA_RESULT_CODE* result, WHV_GUEST_PHYSICAL_ADDRESS* gpa);

    uint32_t PortRead(uint16_t port, uint8_t size);
    void PortWrite(uint16_t port, uint8_t size, uint32_t value);
    uint64_t MmioRead(uint64_t gpa, uint8_t size);
    void MmioWrite(uint64_t gpa, uint8_t size, uint64_t value);

    WHV_PARTITION_HANDLE partition_ = nullptr;
    WHV_EMULATOR_HANDLE emulator_ = nullptr;
    GuestMemory* mem_ = nullptr;
    uint32_t cpu_count_ = 1;
    std::map<uint16_t, PortDevice*> ports_;
    IoApic* ioapic_ = nullptr;
    PciBus* pci_ = nullptr;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> stop_requested_{false};
    std::mutex stop_mutex_;
    std::string stop_reason_;
    std::function<void(const std::string&)> on_stop_;
};

}  // namespace Aetheron
