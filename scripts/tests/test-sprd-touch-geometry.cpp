// SPDX-License-Identifier: GPL-2.0-only
#include "../../drivers/SprdPlatform/SprdTouchGeometry.h"
#include <assert.h>
#include <stdio.h>

using namespace SprdBus;
int main() {
    const Contact raw[]={{1,75,1119},{3,109,23},{8,1879,1142},{15,1869,54}};
    const Contact expected[]={{1,80,75},{3,1176,109},{8,57,1879},{15,1145,1869}};
    Frame input={},output={};input.count=4;
    for(unsigned i=0;i<4;i++)input.contacts[i]=raw[i];
    assert(a2GoodixPanelFrame(input,1920,1200,output)==Ok);
    assert(output.count==4);
    for(unsigned i=0;i<4;i++) {
        assert(output.contacts[i].id==expected[i].id);
        assert(output.contacts[i].x==expected[i].x);
        assert(output.contacts[i].y==expected[i].y);
    }
    assert(output.contacts[0].x<600 && output.contacts[0].y<960);
    assert(output.contacts[1].x>=600 && output.contacts[1].y<960);
    assert(output.contacts[2].x<600 && output.contacts[2].y>=960);
    assert(output.contacts[3].x>=600 && output.contacts[3].y>=960);
    input.count=0;
    assert(a2GoodixPanelFrame(input,1920,1200,output)==Ok && output.count==0);
    input.count=10;
    for(unsigned i=0;i<10;i++)input.contacts[i]={(uint8_t)i,(uint16_t)(i*200),(uint16_t)(i*120)};
    assert(a2GoodixPanelFrame(input,1920,1200,output)==Ok && output.count==10);
    input.count=1;input.contacts[0]={0,1919,1199};
    assert(a2GoodixPanelFrame(input,1920,1200,output)==Ok);
    assert(output.contacts[0].x==0 && output.contacts[0].y==1919);
    input.contacts[0]={0,0,0};
    assert(a2GoodixPanelFrame(input,1920,1200,output)==Ok);
    assert(output.contacts[0].x==1199 && output.contacts[0].y==0);
    input.contacts[0].x=1920;
    assert(a2GoodixPanelFrame(input,1920,1200,output)==Protocol);
    input.contacts[0]={0,0,1200};
    assert(a2GoodixPanelFrame(input,1920,1200,output)==Protocol);
    input.count=11;
    assert(a2GoodixPanelFrame(input,1920,1200,output)==Invalid);
    input.count=0;
    assert(a2GoodixPanelFrame(input,1200,1920,output)==Invalid);
    puts("PASS: four measured panel corners, contact IDs, zero-contact release, ten contacts, edges, invalid dimensions and bounds");
}
