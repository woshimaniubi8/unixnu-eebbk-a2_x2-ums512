// SPDX-License-Identifier: GPL-2.0-only
#include "../../drivers/SprdPlatform/SprdBus.h"
#include "../../drivers/SprdPlatform/SprdHomeButton.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <vector>

using namespace SprdKeys;

struct Rig {
    SprdBus::Debounce debounce;
    HomeChord mapper;
    uint64_t now = 0;
    uint8_t last = 0;
    uint8_t homePresses = 0;
    std::vector<uint8_t> events;

    void accept(uint8_t report) {
        if (last == report) return;
        if (report & Home) ++homePresses;
        last = report;
        events.push_back(report);
    }
    void hold(uint8_t raw, unsigned ms) {
        assert(ms % 10 == 0);
        for (unsigned elapsed = 0; elapsed < ms; elapsed += 10) {
            now += 10;
            debounce.sample(raw);
            accept(mapper.sample(raw, static_cast<uint8_t>(debounce.stable), now));
        }
    }
    void miss(unsigned ms) {
        now += ms;
        accept(mapper.missed(now));
    }
    void expect(std::initializer_list<uint8_t> expected) const {
        assert(events == std::vector<uint8_t>(expected));
    }
};

int main() {
    constexpr uint8_t chord = HomeChord::Keys;
    // Single-key taps, including ones released before the pairing window.
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(VolumeDown, 90);
        rig.hold(0, 110);
        rig.hold(Power, 100);
        rig.hold(0, 110);
        rig.hold(VolumeUp, 100);
        rig.hold(0, 110);
        rig.expect({VolumeDown, 0, Power, 0, VolumeUp, 0});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(chord, 3600);
        // The long press by itself cannot unlock or trigger an event.
        rig.expect({});
        rig.hold(0, 30);
        rig.expect({Home});
        rig.hold(0, 60);
        rig.expect({Home});
        rig.hold(0, 20);
        rig.expect({Home, 0});
    }
    // Distinguish a short combo from a recovery-length hold. Neither sends
    // Home or taps the power key after the gesture had claimed the combo.
    for (unsigned ms : {290, 2990, 3000, 3380, 5000, 5010, 13500}) {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(chord, ms);
        rig.hold(0, 110);
        if (ms >= HomeChord::MinHoldMs && ms <= HomeChord::MaxHoldMs) {
            rig.expect({Home, 0});
        } else {
            rig.expect({});
        }
    }
    {
        Rig rig;
        rig.hold(chord, 3500);
        rig.hold(0, 110);
        rig.expect({chord, 0}); // Held at start, never Home.
        rig.hold(chord, 3500);
        rig.hold(0, 110);
        rig.expect({chord, 0, Home, 0});
    }
    {
        // Staggered but near-simultaneous press/release.
        Rig rig;
        rig.hold(0, 40);
        rig.hold(Power, 100);
        rig.hold(chord, 3350);
        rig.hold(VolumeDown, 180);
        rig.expect({});
        rig.hold(0, 110);
        rig.expect({Home, 0});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(chord, 3500);
        rig.hold(VolumeDown, 500);
        rig.hold(0, 110);
        rig.expect({});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(chord, 3300);
        rig.hold(VolumeDown, 100);
        rig.hold(chord, 100);
        rig.hold(0, 110);
        rig.expect({});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(VolumeDown, 900);
        rig.expect({VolumeDown}); // Single long holds still reach the OS.
        rig.hold(chord, 3500);
        rig.hold(0, 110);
        rig.expect({VolumeDown, chord, 0});
        rig.hold(Power | VolumeUp, 3500);
        rig.hold(0, 110);
        rig.expect({VolumeDown, chord, 0, Power | VolumeUp, 0});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(chord, 2200);
        rig.hold(chord | VolumeUp, 1000);
        rig.hold(chord, 850);
        rig.hold(0, 110);
        rig.expect({VolumeUp, 0});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(chord, 2800);
        rig.miss(500);
        rig.hold(chord, 100);
        rig.hold(0, 110);
        rig.expect({});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(chord, 2700);
        rig.now += HomeChord::MaxSampleGapMs;
        rig.hold(chord, 100);
        rig.hold(0, 110);
        rig.expect({});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(chord, 3450);
        rig.hold(0, 30);
        rig.miss(HomeChord::PulseMs);
        rig.expect({Home, 0});
    }
    std::puts("PASS: single keys, 3-5 s Home chord, staggered release, no boot/recovery trigger, polling errors");
}
