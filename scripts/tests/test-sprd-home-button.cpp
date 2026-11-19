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
    VolumeHome mapper;
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
    // Normal taps are not lost while distinguishing a hold.
    {
        Rig rig;
        rig.hold(0, 40);
        for (unsigned i = 0; i < 2; ++i) {
            rig.hold(VolumeDown, 90);
            rig.hold(0, 110);
        }
        rig.expect({VolumeDown, 0, VolumeDown, 0});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(VolumeDown, 1800);
        rig.expect({});
        rig.hold(0, 30);
        rig.expect({Home});
        rig.hold(0, 60);
        rig.expect({Home});
        rig.hold(0, 20);
        rig.expect({Home, 0});
    }
    // A held key when starting is only a physical press, not a Home gesture.
    {
        Rig rig;
        rig.hold(VolumeDown, 1500);
        rig.hold(0, 100);
        rig.expect({VolumeDown, 0});
        rig.hold(VolumeDown, 850);
        rig.hold(0, 110);
        rig.expect({VolumeDown, 0, Home, 0});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(Power, 100);
        rig.hold(0, 50);
        rig.hold(VolumeUp, 100);
        rig.hold(0, 50);
        rig.hold(Power | VolumeDown, 1100);
        rig.hold(VolumeDown, 1000);
        rig.hold(0, 110);
        rig.expect({Power, 0, VolumeUp, 0, Power | VolumeDown, VolumeDown, 0});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(VolumeDown, 500);
        // A power key arriving even before its debounced edge cancels Home.
        rig.hold(VolumeDown | Power, 10);
        rig.hold(VolumeDown, 600);
        rig.hold(0, 110);
        assert(rig.homePresses == 0);
    }
    // Time thresholds on a continuously sampled press.
    for (unsigned ms : {790, 800, 900}) {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(VolumeDown, ms);
        rig.hold(0, 110);
        rig.expect({ms >= VolumeHome::HoldMs ? Home : VolumeDown, 0});
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(VolumeDown, 720);
        rig.miss(500);
        rig.hold(VolumeDown, 100);
        rig.hold(0, 110);
        assert(rig.homePresses == 0);
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(VolumeDown, 500);
        rig.now += VolumeHome::MaxSampleGapMs;
        rig.hold(VolumeDown, 100);
        rig.hold(0, 110);
        assert(rig.homePresses == 0);
    }
    {
        Rig rig;
        rig.hold(0, 40);
        rig.hold(VolumeDown, 900);
        rig.hold(0, 30);
        rig.miss(VolumeHome::PulseMs);
        rig.expect({Home, 0});
    }
    std::puts("PASS: volume tap, Home hold/release, held-at-boot, power/volume combos, threshold and polling errors");
}
