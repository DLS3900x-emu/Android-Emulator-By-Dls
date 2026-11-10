#include "uart16550.h"

namespace Aetheron {

Uart16550::Uart16550(uint16_t base, uint32_t gsi, IrqSink* irq, const char* log_path)
    : base_(base), gsi_(gsi), irq_(irq) {
    log_ = fopen(log_path, "wb");
}

Uart16550::~Uart16550() {
    if (log_) fclose(log_);
}

uint8_t Uart16550::Iir() const {
    uint8_t fifo = (fcr_ & 1) ? 0xC0 : 0;
    if ((ier_ & 0x01) && !rx_.empty()) return fifo | 0x04;  // received data available
    if ((ier_ & 0x02) && thr_irq_pending_) return fifo | 0x02;  // THR empty
    return fifo | 0x01;  // no interrupt pending
}

void Uart16550::UpdateIrq() {
    bool want = (Iir() & 0x01) == 0 && (mcr_ & 0x08);  // OUT2 gates the IRQ on PCs
    if (want == irq_line_) return;
    irq_line_ = want;
    // ISA IRQs are edge-triggered on the IOAPIC: raise on 0->1, drop on 1->0.
    if (irq_) irq_->SetIrqLevel(gsi_, want);
}

uint32_t Uart16550::PortRead(uint16_t port, uint8_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint16_t reg = port - base_;
    bool dlab = (lcr_ & 0x80) != 0;
    uint8_t v = 0;
    switch (reg) {
        case 0:
            if (dlab) {
                v = dll_;
            } else if (!rx_.empty()) {
                v = rx_.front();
                rx_.pop_front();
            }
            break;
        case 1:
            v = dlab ? dlm_ : ier_;
            break;
        case 2:
            v = Iir();
            if ((v & 0x0F) == 0x02) thr_irq_pending_ = false;  // reading IIR clears THRE source
            break;
        case 3:
            v = lcr_;
            break;
        case 4:
            v = mcr_;
            break;
        case 5:
            v = 0x60 | (rx_.empty() ? 0 : 0x01);  // THRE | TEMT | DR
            break;
        case 6:
            v = (mcr_ & 0x10) ? uint8_t(((mcr_ & 0x0F) << 4))  // loopback
                              : 0xB0;                         // DCD | DSR | CTS
            break;
        case 7:
            v = scr_;
            break;
    }
    UpdateIrq();
    return v;
}

void Uart16550::PortWrite(uint16_t port, uint8_t size, uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint16_t reg = port - base_;
    bool dlab = (lcr_ & 0x80) != 0;
    uint8_t v = uint8_t(value);
    switch (reg) {
        case 0:
            if (dlab) {
                dll_ = v;
            } else {
                if (mcr_ & 0x10) {
                    rx_.push_back(v);  // loopback mode
                } else {
                    if (log_) {
                        fputc(v, log_);
                        if (v == '\n') fflush(log_);
                    }
                    if (on_tx_) on_tx_(char(v));
                }
                thr_irq_pending_ = true;  // transmit completes instantly
            }
            break;
        case 1:
            if (dlab) {
                dlm_ = v;
            } else {
                bool thri_enabled = !(ier_ & 0x02) && (v & 0x02);
                ier_ = v & 0x0F;
                if (thri_enabled) thr_irq_pending_ = true;
            }
            break;
        case 2:
            fcr_ = v;
            if (v & 0x02) rx_.clear();
            break;
        case 3:
            lcr_ = v;
            break;
        case 4:
            mcr_ = v & 0x1F;
            break;
        case 7:
            scr_ = v;
            break;
    }
    UpdateIrq();
}

void Uart16550::InjectInput(const char* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < len; ++i) rx_.push_back(uint8_t(data[i]));
    UpdateIrq();
}

}  // namespace Aetheron
