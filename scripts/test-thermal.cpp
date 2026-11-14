#include <cassert>
#include <cstdint>
#include "../drivers/SprdThermal/ThermalMath.h"
int main() {
    using namespace SprdThermalMath;
    Calibration c{}; int32_t t=0;
    assert(calibrate(0,0,128,c) && c.slope==262 && c.offset==66400);
    assert(convert(c,400,t) && t==38400 && fixedC(t)==2516582);
    assert(convert(c,800,t) && t==143200); // real heat must reach critical handling
    assert(!convert(c,1023,t));
    assert(!calibrate(2,0,128,c)); assert(!calibrate(0,128,128,c));
    assert(!calibrate(0,0,256,c));
    for (uint32_t sign=0;sign<2;sign++) for(uint32_t ratio=0;ratio<128;ratio++)
        for(uint32_t delta=0;delta<256;delta++) {
            assert(calibrate(sign,ratio,delta,c));
            int expectedRatio=sign ? 1000-(int)ratio : 1000+(int)ratio;
            assert(c.slope==262*expectedRatio/1000);
            assert(c.offset==66400+((int)delta-128)*250);
            auto raw=threshold(c,110000); assert(raw<=999);
        }
}
