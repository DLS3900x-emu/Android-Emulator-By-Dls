// Derives the guest's boot stage from the real kernel/init/logcat output
// (no timers, no guessing): each stage is reported only when the matching
// log line actually appears on ttyS0 or on the logcat console (hvc2).
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace Aetheron {

class BootStageTracker {
public:
    enum Channel { kSerial = 0, kLogcat = 1 };
    static constexpr int kRankSecondStage = 4;

    explicit BootStageTracker(std::function<void(const std::string&)> on_stage) : on_stage_(std::move(on_stage)) {}

    void Feed(char c, Channel ch = kSerial) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string& line = line_[ch];
        if (c == '\n') {
            Line(line);
            line.clear();
        } else if (c != '\r' && line.size() < 1024) {
            line.push_back(c);
        }
    }

    int rank() const { return rank_; }
    // True once the kernel printed that it halted or powered down.
    bool halted() const { return halted_; }

private:
    void Line(const std::string& l) {
        struct Marker {
            int rank;
            const char* text;
            const char* stage;
        };
        static const Marker kMarkers[] = {
            {1, "Linux version", "Linux kernel starting"},
            {2, "Trying to unpack rootfs", "kernel: unpacking ramdisk"},
            {3, "Run /init as init process", "Android first-stage init"},
            {4, "init second stage started", "Android init (second stage)"},
            {5, "starting service 'zygote'", "Android framework starting (zygote)"},
            {6, "Entered the Android system server", "Android system server starting"},
            {7, "sys.boot_completed=1", "Android boot completed"},
            {7, "Boot is finished", "Android boot completed"},
        };
        for (const auto& m : kMarkers) {
            if (m.rank > rank_ && l.find(m.text) != std::string::npos) {
                rank_ = m.rank;
                on_stage_(m.stage);
            }
        }
        if (l.find("Kernel panic") != std::string::npos) on_stage_("KERNEL PANIC (see serial.log)");
        if (l.find("reboot: System halted") != std::string::npos || l.find("reboot: Power down") != std::string::npos)
            halted_ = true;
    }

    std::function<void(const std::string&)> on_stage_;
    std::mutex mutex_;
    std::string line_[2];
    std::atomic<int> rank_{0};
    std::atomic<bool> halted_{false};
};

}  // namespace Aetheron
