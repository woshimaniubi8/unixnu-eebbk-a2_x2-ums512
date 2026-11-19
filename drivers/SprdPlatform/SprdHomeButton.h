// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <stdint.h>

namespace SprdKeys {
// Consumer report 2: Menu (Home), Power, Volume Increment, Volume Decrement.
constexpr uint8_t Home = 1u << 0;
constexpr uint8_t Power = 1u << 1;
constexpr uint8_t VolumeUp = 1u << 2;
constexpr uint8_t VolumeDown = 1u << 3;
constexpr uint8_t PhysicalMask = Power | VolumeUp | VolumeDown;
constexpr uint8_t ReportMask = PhysicalMask | Home;
constexpr uint8_t ConsumerReportId = 2;

class HomeChord {
public:
    static constexpr uint8_t Keys = Power | VolumeDown;
    static constexpr uint64_t MinHoldMs = 3000;
    static constexpr uint64_t MaxHoldMs = 5000;
    static constexpr uint64_t PairWindowMs = 180;
    static constexpr uint64_t ReleaseWindowMs = 400;
    static constexpr uint64_t PulseMs = 70;
    static constexpr uint64_t MaxSampleGapMs = 250;

    uint8_t sample(uint8_t raw, uint8_t stable, uint64_t nowMs) {
        raw &= PhysicalMask;
        stable &= PhysicalMask;
        if (haveSample_ && (nowMs < lastSample_ || nowMs - lastSample_ > MaxSampleGapMs)) {
            interrupted();
            pulse_ = 0;
        }
        lastSample_ = nowMs;
        haveSample_ = true;

        if (!(raw | stable)) {
            if (phase_ == PendingSingle) {
                // A tap released during the pairing window still produces
                // its original press and release on the consumer report.
                startPulse(single_, nowMs);
            } else if ((phase_ == Holding && qualified(nowMs)) ||
                       (phase_ == Releasing && nowMs - firstRelease_ <= ReleaseWindowMs)) {
                startPulse(Home, nowMs);
            }
            phase_ = Ready;
            return report(0, nowMs);
        }

        if (raw) pulse_ = 0; // A new physical press ends an old pulse.
        if (phase_ == Unarmed || phase_ == Ordinary) {
            return report(stable, nowMs);
        }

        if ((raw | stable) & VolumeUp) {
            // A third key cancels Home. If the chord already owns Power and
            // VolumeDown, do not suddenly replay those held keys to iPadOS.
            if (ownsChord()) return consume(stable, nowMs);
            phase_ = Ordinary;
            return report(stable, nowMs);
        }

        switch (phase_) {
            case Ready:
                if (raw == stable) {
                    if (stable == Keys) beginChord(nowMs);
                    else {
                        single_ = stable;
                        started_ = nowMs;
                        phase_ = PendingSingle;
                    }
                }
                break;
            case PendingSingle:
                if (raw == stable && stable) {
                    if (stable == Keys && nowMs - started_ <= PairWindowMs) {
                        beginChord(nowMs);
                    } else if (stable != single_ || nowMs - started_ >= PairWindowMs) {
                        phase_ = Ordinary;
                        return report(stable, nowMs);
                    }
                }
                break;
            case Holding:
                if (nowMs - started_ > MaxHoldMs) return consume(stable, nowMs);
                if (raw == stable && stable != Keys) {
                    if (!qualified(nowMs)) return consume(stable, nowMs);
                    remaining_ = stable;
                    firstRelease_ = nowMs;
                    phase_ = Releasing;
                }
                break;
            case Releasing:
                if (nowMs - firstRelease_ > ReleaseWindowMs ||
                    ((raw | stable) & ~remaining_)) return consume(stable, nowMs);
                break;
            case Consumed:
                return consume(stable, nowMs);
            case Unarmed:
            case Ordinary:
                break;
        }
        return report(0, nowMs);
    }

    uint8_t missed(uint64_t nowMs) {
        interrupted();
        return report(physical_, nowMs);
    }

private:
    enum Phase { Unarmed, Ready, PendingSingle, Ordinary, Holding, Releasing, Consumed };

    bool ownsChord() const {
        return phase_ == Holding || phase_ == Releasing || phase_ == Consumed;
    }

    void interrupted() {
        // A missed or stale sample cannot qualify a hold. Still expire a
        // previously committed HID pulse, even if ADI remains unavailable.
        phase_ = ownsChord() ? Consumed : Unarmed;
    }

    void beginChord(uint64_t nowMs) {
        started_ = nowMs;
        phase_ = Holding;
    }

    bool qualified(uint64_t nowMs) const {
        const uint64_t duration = nowMs - started_;
        return duration >= MinHoldMs && duration <= MaxHoldMs;
    }

    uint8_t consume(uint8_t stable, uint64_t nowMs) {
        phase_ = Consumed;
        return report(stable & VolumeUp, nowMs);
    }

    void startPulse(uint8_t key, uint64_t nowMs) {
        pulse_ = key;
        pulseStarted_ = nowMs;
    }

    uint8_t report(uint8_t physical, uint64_t nowMs) {
        physical_ = physical;
        if (pulse_ && (nowMs < pulseStarted_ || nowMs - pulseStarted_ >= PulseMs)) {
            pulse_ = 0;
        }
        return physical_ | pulse_;
    }

    Phase phase_ = Unarmed;
    bool haveSample_ = false;
    uint64_t started_ = 0;
    uint64_t firstRelease_ = 0;
    uint64_t lastSample_ = 0;
    uint64_t pulseStarted_ = 0;
    uint8_t single_ = 0;
    uint8_t remaining_ = 0;
    uint8_t physical_ = 0;
    uint8_t pulse_ = 0;
};
}
