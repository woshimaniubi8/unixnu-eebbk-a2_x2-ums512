#!/usr/bin/env python3
"""Decode emitted packets independently using the pinned Mesa XML fields."""
import argparse
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('capture',type=Path);args=parser.parse_args()
xml=ET.parse(ROOT/'hardware/mesa-bifrost/v7.xml').getroot()
enums={node.get('name'):{value.get('name'):int(value.get('value'),0) for value in node.findall('value')} for node in xml.findall('enum')}
structs={node.get('name'):node for node in xml.findall('struct')}

def decode(name,blob):
    bits=int.from_bytes(blob,'little');result={}
    for field in structs[name].findall('field'):
        start=field.get('start').split(':')
        shift=int(start[0])*32+int(start[1]) if len(start)==2 else int(start[0])
        size=int(field.get('size'));value=(bits>>shift)&((1<<size)-1)
        modifier=field.get('modifier','')
        if modifier=='minus(1)':value+=1
        elif modifier=='log2':value=1<<value
        elif modifier.startswith('shr('):value<<=int(modifier[4:-1])
        result[field.get('name')]=value
    return result

def require(packet,**fields):
    for key,value in fields.items():assert packet[key.replace('_',' ')]==value,(key,packet,value)

raw=args.capture.read_bytes();assert len(raw)==4*0x10000+2*0x800
colors=[0xff0000ff,0xff00ff00,0xffff0000,0xffffffff]
for quadrant in range(4):
    data=raw[quadrant*0x10000:(quadrant+1)*0x10000]
    x=32 if quadrant&1 else 0;y=32 if quadrant&2 else 0
    job=decode('Job Header',data[:32])
    require(job,Type=enums['Job Type']['Fragment'],Index=1,Is_64b=1,Next=0,Exception_Status=0)
    payload=decode('Fragment Job Payload',data[32:64])
    require(payload,Bound_Min_X=x//16,Bound_Min_Y=y//16,Bound_Max_X=(x+31)//16,Bound_Max_Y=(y+31)//16,Framebuffer=0x1000101)
    fb=decode('Framebuffer Parameters',data[0x100:0x140])
    require(fb,Width=64,Height=64,Bound_Min_X=x,Bound_Min_Y=y,Bound_Max_X=x+31,Bound_Max_Y=y+31,Sample_Count=1,Effective_Tile_Size=256,Color_Buffer_Allocation=1024,Render_Target_Count=1,Tiler=0x1000240,Sample_Locations=0x1000400)
    rt=decode('RGB Render Target',data[0x180:0x1c0])
    require(rt,Write_Enable=1,Clean_Tile_Write_Enable=1,Writeback_Format=enums['Color Format']['R8G8B8A8'],Writeback_Block_Format=enums['Block Format']['Linear'],Swizzle=0x688)
    buffer=decode('RT Buffer',data[0x1a0:0x1b0])
    require(buffer,Base=0x1008000,Row_Stride=256,Surface_Stride=16384)
    clear=decode('RT Clear',data[0x1b0:0x1c0])
    assert all(value==colors[quadrant] for value in clear.values())
    heap=decode('Tiler Heap',data[0x200:0x220])
    require(heap,Base=0x1002000,Bottom=0x1002000,Top=0x1004000,Size=8192)
    tiler=decode('Tiler Context',data[0x240:0x300])
    require(tiler,Polygon_List=0,Hierarchy_Mask=1,FB_Width=64,FB_Height=64,Heap=0x1000200)
    assert struct.unpack_from('<HH',data,0x400)==(128,128)
    assert data[0x4000:]==bytes([0xa5])*0xc000
print('PASS: independent Mesa XML decode of all four fragment/framebuffer/RT/tiler packets and untouched image/guards')

for index,(x,y,color) in enumerate(((0,0,0xffb3812d),(704,1136,0xff35cc92))):
    data=raw[0x40000+index*0x800:0x40000+(index+1)*0x800]
    job=decode('Job Header',data[:32])
    require(job,Type=enums['Job Type']['Fragment'],Index=1,Is_64b=1,Next=0,Exception_Status=0)
    payload=decode('Fragment Job Payload',data[32:64])
    require(payload,Bound_Min_X=x//16,Bound_Min_Y=y//16,Bound_Max_X=74,Bound_Max_Y=119,
            Has_Tile_Enable_Map=0,Framebuffer=0x2000101)
    fb=decode('Framebuffer Parameters',data[0x100:0x140])
    require(fb,Width=1200,Height=1920,Bound_Min_X=x,Bound_Min_Y=y,Bound_Max_X=1199,Bound_Max_Y=1919,
            Sample_Count=1,Effective_Tile_Size=256,Color_Buffer_Allocation=1024,
            Render_Target_Count=1,Tiler=0x2000240,Sample_Locations=0x2000400)
    rt=decode('RGB Render Target',data[0x180:0x1c0])
    require(rt,Write_Enable=1,Clean_Tile_Write_Enable=1,Writeback_Format=enums['Color Format']['R8G8B8A8'],
            Writeback_Block_Format=enums['Block Format']['Linear'],Swizzle=0x688)
    require(decode('RT Buffer',data[0x1a0:0x1b0]),Base=0x2028000,Row_Stride=4864,Surface_Stride=9338880)
    assert all(value==color for value in decode('RT Clear',data[0x1b0:0x1c0]).values())
    require(decode('Tiler Heap',data[0x200:0x220]),Base=0x2002000,Bottom=0x2002000,Top=0x2022000,Size=131072)
    require(decode('Tiler Context',data[0x240:0x300]),Polygon_List=0,Hierarchy_Mask=1,
            FB_Width=1200,FB_Height=1920,Heap=0x2000200)
    assert struct.unpack_from('<HH',data,0x400)==struct.unpack_from('<HH',data,0x480)==(128,128)
print('PASS: independent Mesa XML decode of full-size and partial frame writeback')
