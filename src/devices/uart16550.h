// 16550A UART (COM1 at 0x3F8, IRQ4). Transmitted bytes go to serial.log.
// Bytes queued with InjectInput() arrive in the guest's receive buffer, so a
// console shell on ttyS0 can be driven from the host.
#pragma once
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>

#include "device.h"

namespace Aetheron {

class Uart16550 : public PortDevice {
public:
    Uart16550(uint16_t base, uint32_t gsi, IrqSink* irq, const char* log_path);
    ~Uart16550() override;

    uint32_t PortRead(uint16_t port, uint8_t size) override;
    void PortWrite(uint16_t port, uint8_t size, uint32_t value) override;

    void InjectInput(const char* data, size_t len);
    // Observer for every byte the guest transmits (called with the UART lock
    // held; it must not call back into the UART).
    void SetTxObserver(std::function<void(char)> fn) { on_tx_ = std::move(fn); }

private:
    void UpdateIrq();  // mutex held
    uint8_t Iir() const;

    std::mutex mutex_;
    uint16_t base_;
    uint32_t gsi_;
    IrqSink* irq_;
    FILE* log_ = nullptr;
    std::function<void(char)> on_tx_;

    uint8_t ier_ = 0, lcr_ = 0, mcr_ = 0, scr_ = 0, fcr_ = 0;
    uint8_t dll_ = 0x0C, dlm_ = 0;
    bool thr_irq_pending_ = false;
    std::deque<uint8_t> rx_;
    bool irq_line_ = false;
};

}  // namespace Aetheron
