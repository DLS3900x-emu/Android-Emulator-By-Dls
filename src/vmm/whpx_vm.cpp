#include "whpx_vm.h"

#include <cstring>

#include "../common/log.h"

namespace Aetheron {

namespace {
std::string Hex(HRESULT hr) {
    char b[16];
    snprintf(b, sizeof(b), "0x%08lx", (unsigned long)hr);
    return b;
}
}  // namespace

WhpxVm::~WhpxVm() {
    Stop();
    if (emulator_) WHvEmulatorDestroyEmulator(emulator_);
    if (partition_) WHvDeletePartition(partition_);
}

bool WhpxVm::IsAvailable(std::string* why) {
    WHV_CAPABILITY cap = {};
    UINT32 written = 0;
    HRESULT hr = WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &cap, sizeof(cap), &written);
    if (FAILED(hr) || !cap.HypervisorPresent) {
        *why = "Windows Hypervisor Platform is not enabled (hr=" + Hex(hr) + ")";
        return false;
    }
    std::memset(&cap, 0, sizeof(cap));
    hr = WHvGetCapability(WHvCapabilityCodeExtendedVmExits, &cap, sizeof(cap), &written);
    if (FAILED(hr) || !cap.ExtendedVmExits.X64CpuidExit) {
        *why = "this Windows build's hypervisor lacks CPUID exits";
        return false;
    }
    return true;
}

bool WhpxVm::Create(uint32_t cpu_count, GuestMemory* mem) {
    mem_ = mem;
    cpu_count_ = cpu_count;
    HRESULT hr = WHvCreatePartition(&partition_);
    if (FAILED(hr)) {
        Log("whpx: WHvCreatePartition failed %s", Hex(hr).c_str());
        return false;
    }
    WHV_PARTITION_PROPERTY p = {};
    p.ProcessorCount = cpu_count;
    hr = WHvSetPartitionProperty(partition_, WHvPartitionPropertyCodeProcessorCount, &p, sizeof(p));
    if (FAILED(hr)) {
        Log("whpx: set ProcessorCount failed %s", Hex(hr).c_str());
        return false;
    }
    std::memset(&p, 0, sizeof(p));
    p.LocalApicEmulationMode = WHvX64LocalApicEmulationModeXApic;
    hr = WHvSetPartitionProperty(partition_, WHvPartitionPropertyCodeLocalApicEmulationMode, &p, sizeof(p));
    if (FAILED(hr)) {
        Log("whpx: in-hypervisor xAPIC emulation unavailable %s", Hex(hr).c_str());
        return false;
    }
    std::memset(&p, 0, sizeof(p));
    p.ExtendedVmExits.X64CpuidExit = 1;
    hr = WHvSetPartitionProperty(partition_, WHvPartitionPropertyCodeExtendedVmExits, &p, sizeof(p));
    if (FAILED(hr)) {
        Log("whpx: set ExtendedVmExits failed %s", Hex(hr).c_str());
        return false;
    }
    // CPUID leaves we adjust (see HandleCpuid); all others run natively.
    std::vector<UINT32> leaves = {0x1, 0xB, 0x1F};
    for (UINT32 l = 0x40000000; l <= 0x40000010; ++l) leaves.push_back(l);
    hr = WHvSetPartitionProperty(partition_, WHvPartitionPropertyCodeCpuidExitList, leaves.data(),
                                 UINT32(leaves.size() * sizeof(UINT32)));
    if (FAILED(hr)) {
        Log("whpx: set CpuidExitList failed %s", Hex(hr).c_str());
        return false;
    }
    hr = WHvSetupPartition(partition_);
    if (FAILED(hr)) {
        Log("whpx: WHvSetupPartition failed %s", Hex(hr).c_str());
        return false;
    }

    auto map = [&](void* host, uint64_t gpa, uint64_t size) {
        HRESULT r = WHvMapGpaRange(partition_, host, gpa, size,
                                   WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);
        if (FAILED(r)) Log("whpx: WHvMapGpaRange(0x%llx, %llu MiB) failed %s", (unsigned long long)gpa,
                           (unsigned long long)(size >> 20), Hex(r).c_str());
        return SUCCEEDED(r);
    };
    if (!map(mem->low_host(), 0, mem->low_size())) return false;
    if (mem->high_size() && !map(mem->high_host(), GuestMemory::kHighRamBase, mem->high_size())) return false;
    Log("whpx: RAM mapped: 0x0-0x%llx and %llu MiB at 4 GiB", (unsigned long long)mem->low_size(),
        (unsigned long long)(mem->high_size() >> 20));

    for (uint32_t i = 0; i < cpu_count; ++i) {
        hr = WHvCreateVirtualProcessor(partition_, i, 0);
        if (FAILED(hr)) {
            Log("whpx: create vCPU %u failed %s", i, Hex(hr).c_str());
            return false;
        }
    }

    WHV_EMULATOR_CALLBACKS cb = {};
    cb.Size = sizeof(cb);
    cb.WHvEmulatorIoPortCallback = EmuIoPort;
    cb.WHvEmulatorMemoryCallback = EmuMemory;
    cb.WHvEmulatorGetVirtualProcessorRegisters = EmuGetRegs;
    cb.WHvEmulatorSetVirtualProcessorRegisters = EmuSetRegs;
    cb.WHvEmulatorTranslateGvaPage = EmuTranslateGva;
    hr = WHvEmulatorCreateEmulator(&cb, &emulator_);
    if (FAILED(hr)) {
        Log("whpx: WHvEmulatorCreateEmulator failed %s", Hex(hr).c_str());
        return false;
    }
    return true;
}

void WhpxVm::RegisterPorts(uint16_t first, uint16_t last, PortDevice* dev) {
    for (uint32_t p = first; p <= last; ++p) ports_[uint16_t(p)] = dev;
}

bool WhpxVm::SetupBootCpu(uint64_t entry, uint64_t boot_params, uint64_t gdt_gpa) {
    WHV_X64_SEGMENT_REGISTER cs = {}, ds = {};
    cs.Base = 0;
    cs.Limit = 0xFFFFFFFF;
    cs.Selector = 0x10;
    cs.Attributes = 0xC09B;  // present, code exec/read, accessed, 32-bit, 4K granularity
    ds.Base = 0;
    ds.Limit = 0xFFFFFFFF;
    ds.Selector = 0x18;
    ds.Attributes = 0xC093;  // present, data read/write, accessed, 32-bit, 4K granularity
    WHV_X64_TABLE_REGISTER gdtr = {}, idtr = {};
    gdtr.Base = gdt_gpa;
    gdtr.Limit = 4 * 8 - 1;

    WHV_REGISTER_NAME names[] = {WHvX64RegisterRip, WHvX64RegisterRsi,  WHvX64RegisterRflags, WHvX64RegisterCr0,
                                 WHvX64RegisterCr4, WHvX64RegisterEfer, WHvX64RegisterCs,     WHvX64RegisterDs,
                                 WHvX64RegisterEs,  WHvX64RegisterFs,   WHvX64RegisterGs,     WHvX64RegisterSs,
                                 WHvX64RegisterGdtr, WHvX64RegisterIdtr, WHvX64RegisterRbp,   WHvX64RegisterRdi,
                                 WHvX64RegisterRbx};
    WHV_REGISTER_VALUE v[17] = {};
    v[0].Reg64 = entry;
    v[1].Reg64 = boot_params;
    v[2].Reg64 = 0x2;         // interrupts disabled
    v[3].Reg64 = 0x00000031;  // PE | ET | NE, paging off
    v[4].Reg64 = 0;
    v[5].Reg64 = 0;
    v[6].Segment = cs;
    for (int i = 7; i <= 11; ++i) v[i].Segment = ds;
    v[12].Table = gdtr;
    v[13].Table = idtr;
    // RBP/RDI/RBX = 0 per boot protocol
    HRESULT hr = WHvSetVirtualProcessorRegisters(partition_, 0, names, 17, v);
    if (FAILED(hr)) {
        Log("whpx: setting boot registers failed %s", Hex(hr).c_str());
        return false;
    }
    return true;
}

void WhpxVm::DeliverInterrupt(const ApicMessage& m) {
    WHV_INTERRUPT_CONTROL ic = {};
    switch (m.delivery_mode) {
        case 0: ic.Type = WHvX64InterruptTypeFixed; break;
        case 1: ic.Type = WHvX64InterruptTypeLowestPriority; break;
        case 4: ic.Type = WHvX64InterruptTypeNmi; break;
        case 5: ic.Type = WHvX64InterruptTypeInit; break;
        default:
            LogOnce("ioapic-dm-" + std::to_string(m.delivery_mode),
                    "IOAPIC: delivery mode %u (vector 0x%02x) not supported, dropped", m.delivery_mode, m.vector);
            return;
    }
    ic.DestinationMode = m.logical_dest ? WHvX64InterruptDestinationModeLogical
                                        : WHvX64InterruptDestinationModePhysical;
    ic.TriggerMode = m.level_triggered ? WHvX64InterruptTriggerModeLevel : WHvX64InterruptTriggerModeEdge;
    ic.Destination = m.destination;
    ic.Vector = m.vector;
    HRESULT hr = WHvRequestInterrupt(partition_, &ic, sizeof(ic));
    if (FAILED(hr)) LogOnce("reqint-fail", "whpx: WHvRequestInterrupt failed %s", Hex(hr).c_str());
}

bool WhpxVm::Start(std::function<void(const std::string&)> on_stop) {
    on_stop_ = std::move(on_stop);
    running_ = true;
    stopping_ = false;
    for (uint32_t i = 0; i < cpu_count_; ++i) threads_.emplace_back(&WhpxVm::VcpuThread, this, i);
    return true;
}

void WhpxVm::Stop() {
    stopping_ = true;
    if (partition_)
        for (uint32_t i = 0; i < cpu_count_; ++i) WHvCancelRunVirtualProcessor(partition_, i, 0);
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
    running_ = false;
}

void WhpxVm::RequestStop(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        if (stop_reason_.empty()) stop_reason_ = reason;
    }
    stop_requested_ = true;
    if (partition_)
        for (uint32_t i = 0; i < cpu_count_; ++i) WHvCancelRunVirtualProcessor(partition_, i, 0);
}

void WhpxVm::VcpuThread(uint32_t index) {
    WHV_RUN_VP_EXIT_CONTEXT ctx;
    std::string reason;
    while (!stopping_ && !stop_requested_) {
        HRESULT hr = WHvRunVirtualProcessor(partition_, index, &ctx, sizeof(ctx));
        if (FAILED(hr)) {
            reason = "WHvRunVirtualProcessor failed " + Hex(hr);
            break;
        }
        if (!HandleExit(index, ctx, &reason)) break;
    }
    if (stop_requested_) {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        reason = stop_reason_;
    } else if (stopping_ && reason.empty()) {
        reason = "stopped by user";
    }
    Log("vcpu%u: stopped: %s", index, reason.c_str());
    running_ = false;
    if (on_stop_) on_stop_(reason);
}

void WhpxVm::HandleCpuid(uint32_t index, const WHV_RUN_VP_EXIT_CONTEXT& ctx) {
    const auto& c = ctx.CpuidAccess;
    uint32_t leaf = uint32_t(c.Rax);
    uint64_t rax = c.DefaultResultRax, rbx = c.DefaultResultRbx, rcx = c.DefaultResultRcx, rdx = c.DefaultResultRdx;
    if (leaf == 1) {
        rcx &= ~(1ULL << 3);   // MONITOR/MWAIT: not virtualised
        rcx &= ~(1ULL << 21);  // x2APIC: partition uses xAPIC emulation mode
        rcx &= ~(1ULL << 24);  // TSC-deadline: use the LAPIC one-shot timer instead
        rcx &= ~(1ULL << 31);  // no hypervisor interface is offered to the guest (see below)
        rbx = (rbx & 0x0000FFFFULL) | (uint64_t(cpu_count_) << 16) | (uint64_t(index) << 24);
        rdx |= 1ULL << 9;      // APIC present
    } else if (leaf == 0xB || leaf == 0x1F) {
        rdx = index;           // x2APIC id must match the APIC id in the MP table
    } else if (leaf >= 0x40000000 && leaf <= 0x40000010) {
        // The Hyper-V enlightenment leaves describe an interface (hypercalls,
        // synthetic MSRs) this partition is not configured to provide.
        rax = rbx = rcx = rdx = 0;
    }
    WHV_REGISTER_NAME names[5] = {WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRcx, WHvX64RegisterRdx,
                                  WHvX64RegisterRip};
    WHV_REGISTER_VALUE v[5] = {};
    v[0].Reg64 = rax;
    v[1].Reg64 = rbx;
    v[2].Reg64 = rcx;
    v[3].Reg64 = rdx;
    v[4].Reg64 = ctx.VpContext.Rip + ctx.VpContext.InstructionLength;
    WHvSetVirtualProcessorRegisters(partition_, index, names, 5, v);
}

bool WhpxVm::HandleExit(uint32_t index, const WHV_RUN_VP_EXIT_CONTEXT& ctx, std::string* reason) {
    VpContext vpc{this, index};
    WHV_EMULATOR_STATUS st;
    switch (ctx.ExitReason) {
        case WHvRunVpExitReasonX64IoPortAccess: {
            HRESULT hr = WHvEmulatorTryIoEmulation(emulator_, &vpc, &ctx.VpContext, &ctx.IoPortAccess, &st);
            if (FAILED(hr) || !st.EmulationSuccessful) {
                *reason = "I/O emulation failed at port 0x" + std::to_string(ctx.IoPortAccess.PortNumber) +
                          " status=" + Hex(HRESULT(st.AsUINT32));
                return false;
            }
            return true;
        }
        case WHvRunVpExitReasonMemoryAccess: {
            HRESULT hr = WHvEmulatorTryMmioEmulation(emulator_, &vpc, &ctx.VpContext, &ctx.MemoryAccess, &st);
            if (FAILED(hr) || !st.EmulationSuccessful) {
                char b[160];
                snprintf(b, sizeof(b), "MMIO emulation failed at GPA 0x%llx RIP 0x%llx status=0x%x",
                         (unsigned long long)ctx.MemoryAccess.Gpa, (unsigned long long)ctx.VpContext.Rip,
                         st.AsUINT32);
                *reason = b;
                return false;
            }
            return true;
        }
        case WHvRunVpExitReasonX64Cpuid:
            HandleCpuid(index, ctx);
            return true;
        case WHvRunVpExitReasonX64ApicEoi:
            if (ioapic_) ioapic_->OnEoi(uint8_t(ctx.ApicEoi.InterruptVector));
            return true;
        case WHvRunVpExitReasonX64Halt:
            // With in-hypervisor APIC emulation the halt is normally handled
            // by the hypervisor; if it is reported, yield and resume.
            SwitchToThread();
            return true;
        case WHvRunVpExitReasonCanceled:
            return !stopping_ && !stop_requested_;
        case WHvRunVpExitReasonUnrecoverableException: {
            char b[96];
            snprintf(b, sizeof(b), "guest triple fault (unrecoverable exception) at RIP 0x%llx",
                     (unsigned long long)ctx.VpContext.Rip);
            *reason = b;
            return false;
        }
        case WHvRunVpExitReasonInvalidVpRegisterValue:
            *reason = "invalid vCPU register state";
            return false;
        case WHvRunVpExitReasonUnsupportedFeature:
            *reason = "guest used a feature the hypervisor does not support (code " +
                      std::to_string(ctx.UnsupportedFeature.FeatureCode) + ")";
            return false;
        default:
            *reason = "unhandled exit reason " + std::to_string(int(ctx.ExitReason));
            return false;
    }
}

uint32_t WhpxVm::PortRead(uint16_t port, uint8_t size) {
    auto it = ports_.find(port);
    if (it != ports_.end()) return it->second->PortRead(port, size);
    if (port != 0x80 && port != 0xED)
        LogOnce("port-r-" + std::to_string(port), "io: read from unclaimed port 0x%04x (returns all-ones)", port);
    return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
}

void WhpxVm::PortWrite(uint16_t port, uint8_t size, uint32_t value) {
    auto it = ports_.find(port);
    if (it != ports_.end()) {
        it->second->PortWrite(port, size, value);
        return;
    }
    // 0x80 / 0xED are POST-code / delay ports; writes are discarded on real PCs too.
    if (port != 0x80 && port != 0xED)
        LogOnce("port-w-" + std::to_string(port), "io: write to unclaimed port 0x%04x", port);
}

uint64_t WhpxVm::MmioRead(uint64_t gpa, uint8_t size) {
    if (ioapic_ && gpa >= IoApic::kBase && gpa < IoApic::kBase + IoApic::kSize)
        return ioapic_->MmioRead(gpa - IoApic::kBase, size);
    uint64_t v = 0;
    if (pci_ && pci_->HandleMmio(gpa, false, size, &v)) return v;
    LogOnce("mmio-r-" + std::to_string(gpa >> 12), "mmio: read from unclaimed GPA 0x%llx (returns all-ones)",
            (unsigned long long)gpa);
    return ~0ULL;
}

void WhpxVm::MmioWrite(uint64_t gpa, uint8_t size, uint64_t value) {
    if (ioapic_ && gpa >= IoApic::kBase && gpa < IoApic::kBase + IoApic::kSize) {
        ioapic_->MmioWrite(gpa - IoApic::kBase, size, value);
        return;
    }
    if (pci_ && pci_->HandleMmio(gpa, true, size, &value)) return;
    LogOnce("mmio-w-" + std::to_string(gpa >> 12), "mmio: write to unclaimed GPA 0x%llx", (unsigned long long)gpa);
}

HRESULT CALLBACK WhpxVm::EmuIoPort(void* ctx, WHV_EMULATOR_IO_ACCESS_INFO* io) {
    auto* c = static_cast<VpContext*>(ctx);
    uint8_t size = uint8_t(io->AccessSize);
    if (io->Direction == 0) {
        uint32_t v = c->vm->PortRead(io->Port, size);
        io->Data = size == 4 ? v : (v & ((1u << (size * 8)) - 1));
    } else {
        c->vm->PortWrite(io->Port, size, io->Data);
    }
    return S_OK;
}

HRESULT CALLBACK WhpxVm::EmuMemory(void* ctx, WHV_EMULATOR_MEMORY_ACCESS_INFO* m) {
    auto* c = static_cast<VpContext*>(ctx);
    // RAM accesses can reach here for string instructions that straddle MMIO.
    if (void* host = c->vm->mem_->Translate(m->GpaAddress, m->AccessSize)) {
        if (m->Direction == 0)
            std::memcpy(m->Data, host, m->AccessSize);
        else
            std::memcpy(host, m->Data, m->AccessSize);
        return S_OK;
    }
    if (m->Direction == 0) {
        uint64_t v = c->vm->MmioRead(m->GpaAddress, m->AccessSize);
        std::memcpy(m->Data, &v, m->AccessSize);
    } else {
        uint64_t v = 0;
        std::memcpy(&v, m->Data, m->AccessSize);
        c->vm->MmioWrite(m->GpaAddress, m->AccessSize, v);
    }
    return S_OK;
}

HRESULT CALLBACK WhpxVm::EmuGetRegs(void* ctx, const WHV_REGISTER_NAME* names, UINT32 count,
                                    WHV_REGISTER_VALUE* values) {
    auto* c = static_cast<VpContext*>(ctx);
    return WHvGetVirtualProcessorRegisters(c->vm->partition_, c->index, names, count, values);
}

HRESULT CALLBACK WhpxVm::EmuSetRegs(void* ctx, const WHV_REGISTER_NAME* names, UINT32 count,
                                    const WHV_REGISTER_VALUE* values) {
    auto* c = static_cast<VpContext*>(ctx);
    return WHvSetVirtualProcessorRegisters(c->vm->partition_, c->index, names, count, values);
}

HRESULT CALLBACK WhpxVm::EmuTranslateGva(void* ctx, WHV_GUEST_VIRTUAL_ADDRESS gva, WHV_TRANSLATE_GVA_FLAGS flags,
                                         WHV_TRANSLATE_GVA_RESULT_CODE* result, WHV_GUEST_PHYSICAL_ADDRESS* gpa) {
    auto* c = static_cast<VpContext*>(ctx);
    WHV_TRANSLATE_GVA_RESULT r = {};
    HRESULT hr = WHvTranslateGva(c->vm->partition_, c->index, gva, flags, &r, gpa);
    if (SUCCEEDED(hr)) *result = r.ResultCode;
    return hr;
}

}  // namespace Aetheron
