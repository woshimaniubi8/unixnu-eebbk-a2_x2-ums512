// SPDX-License-Identifier: GPL-2.0-only
// Probe the public NativeMali records through the real user-space IOKit boundary.
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include "../../drivers/SprdPlatform/mali/NativeMaliABI.h"
#include "../../drivers/SprdPlatform/SprdMaliRender.h"
using io_object_t=mach_port_t;
using io_connect_t=mach_port_t;
using io_service_t=mach_port_t;
extern "C" {
CFMutableDictionaryRef IOServiceMatching(const char *);
CFMutableDictionaryRef IOServiceNameMatching(const char *);
io_service_t IOServiceGetMatchingService(mach_port_t,CFDictionaryRef);
kern_return_t IOServiceOpen(io_service_t,task_t,uint32_t,io_connect_t *);
kern_return_t IOServiceClose(io_connect_t);
kern_return_t IOObjectRelease(io_object_t);
kern_return_t IOConnectCallStructMethod(io_connect_t,uint32_t,const void *,size_t,void *,size_t *);
kern_return_t IOConnectMapMemory64(io_connect_t,uint32_t,task_t,mach_vm_address_t *,mach_vm_size_t *,uint32_t);
kern_return_t IOConnectUnmapMemory64(io_connect_t,uint32_t,task_t,mach_vm_address_t);
}
namespace {
bool call(io_connect_t c,uint32_t method,const void *in,size_t ins,void *out,size_t outs) {
    size_t size=outs;
    const auto kr=IOConnectCallStructMethod(c,method,in,ins,out,&size);
    printf("M154 mali-user: method=%u result=0x%x out=%zu/%zu\n",method,kr,size,outs);
    return kr==KERN_SUCCESS && size==outs;
}
}
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    puts("M154 mali-user: starting actual IOKit client");
    io_service_t service=IO_OBJECT_NULL;
    for(unsigned attempt=0;attempt<60 && !service;++attempt) {
        service=IOServiceGetMatchingService(0,IOServiceNameMatching("NativeMaliGpuService"));
        if(!service)sleep(1);
    }
    if(!service){puts("M154 mali-user: service absent");return 1;}
    io_connect_t connection=IO_OBJECT_NULL;
    const auto opened=IOServiceOpen(service,mach_task_self(),0,&connection);
    IOObjectRelease(service);
    if(opened!=KERN_SUCCESS || !connection) {
        printf("M154 mali-user: open=0x%x\n",opened);return 2;
    }
    int result=0;
    NativeMaliAbiQuery query{},queried{};
    query.abi_version=NativeMaliAbiVersion;
    NativeMaliAbiDeviceInfo info{};
    NativeMaliAbiBoAlloc request{SprdMaliRender::BufferBytes};
    NativeMaliAbiBoAllocReply allocated{};
    NativeMaliAbiBoMapReply mapped{};
    NativeMaliAbiBoCpuMapReply cpuMap{};
    mach_vm_address_t cpuAddress=0;mach_vm_size_t bytes=0;
    if(!call(connection,NativeMaliMethodQuery,&query,sizeof(query),&queried,sizeof(queried)) ||
       !call(connection,NativeMaliMethodGetDeviceInfo,nullptr,0,&info,sizeof(info))) result=3;
    if(!result && (queried.abi_version!=2 || info.gpu_id!=0x74021000 ||
        queried.input_bits!=35 || queried.output_bits!=40))result=4;
    if(!result && !call(connection,NativeMaliMethodBoAlloc,&request,sizeof(request),
        &allocated,sizeof(allocated)))result=5;
    constexpr uint64_t iova=0x1000000;
    NativeMaliAbiBoMap map{allocated.handle,iova,NativeMaliBoRead|NativeMaliBoWrite,0};
    if(!result && !call(connection,NativeMaliMethodBoMap,&map,sizeof(map),&mapped,sizeof(mapped)))result=6;
    NativeMaliAbiBoCpuMap cpuRequest{allocated.handle};
    if(!result && !call(connection,NativeMaliMethodBoCpuMap,&cpuRequest,sizeof(cpuRequest),
        &cpuMap,sizeof(cpuMap)))result=7;
    kern_return_t mapResult=KERN_FAILURE;
    if(!result) {
        mapResult=IOConnectMapMemory64(connection,cpuMap.token,mach_task_self(),&cpuAddress,&bytes,1);
        if(mapResult!=KERN_SUCCESS || !cpuAddress || bytes<SprdMaliRender::BufferBytes)result=8;
    }
    printf("M154 mali-user: mapped result=0x%x bytes=%llu token=%u gpu=0x%llx\n",
        mapResult,(unsigned long long)bytes,cpuMap.token,(unsigned long long)mapped.iova);
    auto *cpu=reinterpret_cast<uint8_t *>(cpuAddress);
    if(!result)memset(cpu,0xa5,SprdMaliRender::BufferBytes);
    for(unsigned pass=0;pass<4 && !result;++pass) {
        if(!SprdMaliRender::build(cpu,SprdMaliRender::BufferBytes,iova,pass)) {result=9;break;}
        NativeMaliAbiSubmit submit{iova,0,0,0,0};NativeMaliAbiSubmitReply submitted{};
        if(!call(connection,NativeMaliMethodSubmit,&submit,sizeof(submit),&submitted,sizeof(submitted)))
            {result=10;break;}
        NativeMaliAbiWaitFence wait{submitted.fence,1000000000};
        if(!call(connection,NativeMaliMethodWaitFence,&wait,sizeof(wait),nullptr,0))
            {result=11;break;}
        printf("M154 mali-user: fragment pass=%u fence=0x%llx completed\n",pass,(unsigned long long)submitted.fence);
    }
    unsigned mismatch=0,guards=0;
    if(!result) {
        auto *pixels=reinterpret_cast<volatile uint32_t *>(cpu+SprdMaliRender::ImageOffset);
        for(unsigned y=0;y<SprdMaliRender::Height;++y)
            for(unsigned x=0;x<SprdMaliRender::Width;++x)
                mismatch+=pixels[y*SprdMaliRender::Width+x]!=SprdMaliRender::expected(x,y);
        for(size_t p=0x4000;p<SprdMaliRender::ImageOffset;++p)guards+=cpu[p]!=0xa5;
        for(size_t p=SprdMaliRender::ImageOffset+SprdMaliRender::ImageBytes;
            p<SprdMaliRender::BufferBytes;++p)guards+=cpu[p]!=0xa5;
        if(mismatch || guards)result=12;
    }
    NativeMaliAbiStatus status{};
    if(!result && !call(connection,NativeMaliMethodGetStatus,nullptr,0,&status,sizeof(status)))result=13;
    if(cpuAddress) {
        const auto unmapped=IOConnectUnmapMemory64(connection,cpuMap.token,mach_task_self(),cpuAddress);
        if(!result && unmapped!=KERN_SUCCESS)result=14;
    }
    if(cpuMap.token) {
        NativeMaliAbiBoCpuUnmap unmap{allocated.handle,cpuMap.token,0};
        if(!call(connection,NativeMaliMethodBoCpuUnmap,&unmap,sizeof(unmap),nullptr,0) && !result)result=15;
    }
    if(allocated.handle) {
        NativeMaliAbiBoFree free{allocated.handle};
        if(!call(connection,NativeMaliMethodBoFree,&free,sizeof(free),nullptr,0) && !result)result=16;
    }
    const auto closed=IOServiceClose(connection);
    if(closed!=KERN_SUCCESS && !result)result=17;
    printf("M154 mali-user: image result=%d pixels=4096 mismatches=%u guards=%u faults=%llu resets=%llu close=0x%x\n",
        result,mismatch,guards,(unsigned long long)status.faults,(unsigned long long)status.resets,closed);
    return result;
}
