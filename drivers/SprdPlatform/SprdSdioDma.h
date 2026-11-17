// SPDX-License-Identifier: GPL-2.0-only
// Use the same hardware-qualified SDHCR11 ADMA64 descriptor layout as the
// board's eMMC driver. Buffers are private to SDIO1, below4GiB and prepared.
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include "SprdSdio.h"
class SprdSdioDma final:public SprdSdio::Dma {
    IOBufferMemoryDescriptor *table=nullptr,*buffer=nullptr;
    uint64_t tablePa=0,bufferPa=0;bool quarantined=false;
    static IOBufferMemoryDescriptor *allocate(uint64_t &pa){
        auto *b=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),0x4000,0xffffffffULL);
        if(!b)return nullptr;
        if(b->prepare()!=kIOReturnSuccess){b->release();return nullptr;}
        IOByteCount length=0;pa=b->getPhysicalSegment(0,&length);
        if(!pa || (pa&0x3fff) || length<0x4000 || pa>0xffffc000ULL){b->complete();b->release();return nullptr;}
        return b;
    }
    static void sync(void *p,size_t n,bool invalidate){
        uint64_t ctr=0;__asm__ volatile("mrs %0,ctr_el0":"=r"(ctr));
        size_t line=4u<<((ctr>>16)&15);uintptr_t end=(uintptr_t)p+n;
        for(uintptr_t at=(uintptr_t)p&~(line-1);at<end;at+=line){
            if(invalidate)__asm__ volatile("dc civac,%0"::"r"(at):"memory");
            else __asm__ volatile("dc cvac,%0"::"r"(at):"memory");
        }__asm__ volatile("dsb sy":::"memory");
    }
    static void release(IOBufferMemoryDescriptor *d){if(d){d->complete();d->release();}}
public:
    ~SprdSdioDma() override{
        if(!quarantined){release(table);release(buffer);}
    }
    bool initialize(){
        table=allocate(tablePa);buffer=allocate(bufferPa);
        IOLog("M120 sdio: private ADMA table=0x%llx data=0x%llx ready=%u\n",(unsigned long long)tablePa,(unsigned long long)bufferPa,table && buffer);
        return table && buffer;
    }
    SprdBus::Result prepare(uint8_t *data,size_t bytes,bool write,uint64_t &address) override{
        if(!table || !buffer || quarantined || !data || !bytes || bytes>512)return SprdBus::Invalid;
        uint32_t *words=(uint32_t *)table->getBytesNoCopy();
        words[0]=(uint32_t)(bytes<<16)|0x23;words[1]=(uint32_t)bufferPa;words[2]=(uint32_t)(bufferPa>>32);words[3]=0;
        bzero(buffer->getBytesNoCopy(),0x4000);
        if(write)memcpy(buffer->getBytesNoCopy(),data,bytes);
        sync(table->getBytesNoCopy(),16,false);sync(buffer->getBytesNoCopy(),bytes,true);
        address=tablePa;return SprdBus::Ok;
    }
    void complete(uint8_t *data,size_t bytes,bool write,bool success,bool stopped) override{
        if(!stopped){quarantined=true;IOLog("M120 sdio: buffers pinned after unconfirmed command/data reset\n");return;}
        if(success){sync(buffer->getBytesNoCopy(),bytes,true);if(!write)memcpy(data,buffer->getBytesNoCopy(),bytes);}
    }
};
