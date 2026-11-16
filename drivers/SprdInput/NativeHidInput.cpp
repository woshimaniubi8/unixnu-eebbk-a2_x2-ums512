// SPDX-License-Identifier: BSD-2-Clause-Patent
// IOHIDDevice frontend adapted from UniXNU/ShengHidInput. Physical data comes
// only from SprdPlatform; no synthetic Home key or automatic input sequence.
#include "OriginalHidDeviceABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/libkern.h>
#include "../SprdPlatform/SprdBus.h"

static const uint8_t keysDescriptor[]={
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,0x75,0x08,0x95,0x01,0x81,0x03,
    0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,0x75,0x08,0x95,0x06,0x81,0x00,0xc0,
    0x05,0x0c,0x09,0x01,0xa1,0x01,0x85,0x02,
    0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x04,
    0x09,0x40,0x09,0x30,0x09,0xe9,0x09,0xea,0x81,0x02,
    0x75,0x04,0x95,0x01,0x81,0x03,0xc0
};
#define FINGER_DESC \
    0x09,0x22,0xa1,0x02,0x09,0x42,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x81,0x02, \
    0x09,0x47,0x81,0x02,0x75,0x06,0x95,0x01,0x81,0x03, \
    0x09,0x51,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02, \
    0x05,0x01,0x09,0x30,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x01,0x81,0x02, \
    0x09,0x31,0x81,0x02,0x05,0x0d,0xc0,
static const uint8_t touchDescriptor[]={
    0x05,0x0d,0x09,0x04,0xa1,0x01,0x85,0x03,
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    0x09,0x54,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02,0xc0
};

class NativeHidInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeHidInput)
    IOWorkLoop *loop=nullptr;
    IOBufferMemoryDescriptor *report=nullptr;
    bool touchOnly=false,matched=false,sentKeys=false;
    uint8_t lastKeys=0;
    uint32_t attempts=0,notReady=0;IOReturn lastResult=kIOReturnNotReady;
    SprdBus::Frame previous={};
    bool hasDriver(IORegistryEntry *p,unsigned depth) {
        if(!depth)return false;
        OSIterator *it=p->getChildIterator(gIOServicePlane);if(!it)return false;
        bool found=false;
        while(OSObject *o=it->getNextObject()) {
            auto *c=OSDynamicCast(IORegistryEntry,o);
            if(c && (c->metaCast("IOHIDEventDriver") || hasDriver(c,depth-1))){found=true;break;}
        }
        it->release();return found;
    }
    bool ready() {
        if(!matched && hasDriver(this,4)) {
            matched=true;IOLog("bbk input: HID event driver attached kind=%s\n",touchOnly?"touch":"keys");
        }
        return matched;
    }
    IOReturn send(const uint8_t *bytes,size_t length) {
        if(!report || length>report->getLength())return kIOReturnBadArgument;
        if(report->writeBytes(0,bytes,length)!=length)return kIOReturnUnderrun;
        report->setLength(length);IOReturn r=handleReport(report,0,0);attempts++;lastResult=r;return r;
    }
public:
    void configure(bool touch) {touchOnly=touch;}
    IOWorkLoop *getWorkLoop() const override {return loop;}
    OSString *newTransportString() const override {return OSString::withCString(touchOnly?"I2C":"Internal");}
    OSString *newProductString() const override {return OSString::withCString(touchOnly?"EEBBK A2 touch digitizer":"EEBBK A2 physical keys");}
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        if(!out)return kIOReturnBadArgument;
        *out=IOBufferMemoryDescriptor::withBytes(touchOnly?touchDescriptor:keysDescriptor,
                  touchOnly?sizeof(touchDescriptor):sizeof(keysDescriptor),kIODirectionNone);
        return *out?kIOReturnSuccess:kIOReturnNoMemory;
    }
    bool start(IOService *p) override {
        loop=IOWorkLoop::workLoop();
        const uint8_t zero[62]={};
        report=IOBufferMemoryDescriptor::withBytes(zero,touchOnly?sizeof(zero):2,kIODirectionNone);
        if(!loop || !report)return false;
        setProperty("HIDDefaultBehavior",true);setProperty("Built-In",true);
        setProperty("VendorID",0x1782u,32);setProperty("ProductID",touchOnly?0x1902u:0x1901u,32);
        if(touchOnly){setProperty("DisplayIntegrated",true);setProperty("DigitizerCollectionDispatch",true);}
        return IOHIDDevice::start(p);
    }
    void stop(IOService *p) override {IOHIDDevice::stop(p);}
    void free() override {OSSafeReleaseNULL(report);OSSafeReleaseNULL(loop);IOHIDDevice::free();}
    IOReturn buttons(uint8_t keys) {
        if(touchOnly || !ready()){notReady++;return kIOReturnNotReady;}
        keys&=14; // Physical board has Power/Vol+/Vol-, no Home GPIO.
        if(sentKeys && keys==lastKeys)return kIOReturnSuccess;
        uint8_t bytes[]={2,keys};IOReturn r=send(bytes,sizeof(bytes));
        if(r==kIOReturnSuccess){sentKeys=true;lastKeys=keys;IOLog("bbk input: keys mask=0x%x report=0x%x\n",keys,r);}
        return r;
    }
    void describeTree(IORegistryEntry *entry,unsigned depth,unsigned &budget) {
        if(!depth || !budget)return;
        OSIterator *it=entry->getChildIterator(gIOServicePlane);if(!it)return;
        while(OSObject *o=it->getNextObject()) {
            auto *child=OSDynamicCast(IORegistryEntry,o);if(!child)continue;
            IOLog("M102 hid-child: depth=%u name=%s class=%s id=0x%llx\n",depth,child->getName(),child->getMetaClass()->getClassName(),child->getRegistryEntryID());
            if(!--budget)break;describeTree(child,depth-1,budget);
            if(!budget)break;
        }
        it->release();
    }
    void diagnostics() {
        bool found=ready();
        IOLog("M102 hid: kind=%s id=0x%llx eventDriver=%u attempts=%u notReady=%u last=0x%x keys=0x%x\n",
            touchOnly?"touch":"keys",getRegistryEntryID(),found,attempts,notReady,lastResult,lastKeys);
        unsigned budget=12;describeTree(this,4,budget);
    }
    IOReturn contacts(const SprdBus::Frame &frame,unsigned width,unsigned height) {
        if(!touchOnly || !ready()){notReady++;return kIOReturnNotReady;}
        if(frame.count>10 || width<2 || height<2)return kIOReturnBadArgument;
        // Reuse the source-proven per-contact release convention. A report can
        // hold ten contacts; when all IDs change, release the old set first.
        unsigned missing=0;
        for(unsigned i=0;i<previous.count;i++) {
            bool kept=false;for(unsigned j=0;j<frame.count;j++)kept|=previous.contacts[i].id==frame.contacts[j].id;
            missing+=!kept;
        }
        if(frame.count+missing>10) {
            uint8_t release[62]={3};
            for(unsigned i=0;i<previous.count;i++) {
                auto c=previous.contacts[i];uint8_t *p=release+1+i*6;
                p[0]=2;p[1]=c.id;p[2]=(uint8_t)c.x;p[3]=(uint8_t)(c.x>>8);p[4]=(uint8_t)c.y;p[5]=(uint8_t)(c.y>>8);
            }
            release[61]=(uint8_t)previous.count;IOReturn r=send(release,sizeof(release));
            if(r!=kIOReturnSuccess)return r;previous.count=0;
        }
        if(!frame.count && !previous.count)return kIOReturnSuccess;
        uint8_t bytes[62]={3};SprdBus::Frame normalized={};normalized.count=frame.count;
        for(unsigned i=0;i<frame.count;i++) {
            auto c=frame.contacts[i];uint8_t *p=bytes+1+i*6;
            c.x=(uint16_t)((uint32_t)c.x*32767/(width-1));c.y=(uint16_t)((uint32_t)c.y*32767/(height-1));
            p[0]=3;p[1]=c.id;p[2]=(uint8_t)c.x;p[3]=(uint8_t)(c.x>>8);p[4]=(uint8_t)c.y;p[5]=(uint8_t)(c.y>>8);
            normalized.contacts[i]=c;
        }
        unsigned slots=frame.count;
        for(unsigned i=0;i<previous.count;i++) {
            auto c=previous.contacts[i];bool kept=false;
            for(unsigned j=0;j<frame.count;j++)kept|=frame.contacts[j].id==c.id;
            if(kept)continue;
            uint8_t *p=bytes+1+slots++*6;p[0]=2;p[1]=c.id;
            p[2]=(uint8_t)c.x;p[3]=(uint8_t)(c.x>>8);p[4]=(uint8_t)c.y;p[5]=(uint8_t)(c.y>>8);
        }
        bytes[61]=(uint8_t)slots;IOReturn r=send(bytes,sizeof(bytes));
        if(r==kIOReturnSuccess)previous=normalized;return r;
    }
};
OSDefineMetaClassAndStructors(NativeHidInput,IOHIDDevice)
extern "C" IOService *sprd_hid_allocate(bool touch) {auto *d=new NativeHidInput;if(d)d->configure(touch);return d;}
extern "C" IOReturn sprd_hid_buttons(IOService *d,uint8_t mask) {return static_cast<NativeHidInput *>(d)->buttons(mask);}
extern "C" void sprd_hid_diagnostics(IOService *d) {static_cast<NativeHidInput *>(d)->diagnostics();}
extern "C" IOReturn sprd_hid_contacts(IOService *d,const SprdBus::Frame *f,unsigned w,unsigned h) {
    return f?static_cast<NativeHidInput *>(d)->contacts(*f,w,h):kIOReturnBadArgument;
}
