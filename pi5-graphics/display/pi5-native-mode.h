/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include "pi5-display-modes.h"

namespace pi5display {
// BCM2712 register layout and signal requirements checked against the local
// vc4 HDMI/PV/PHY hardware reference. Only progressive 8-bit RGB is generated.
// The first PHY drive-strength range ends at 222 MHz; higher clocks require
// additional analog configurations and, above 340 MHz, SCDC negotiation.
inline bool NativeSupported(const Timing&t){
    return Supported(t)&&t.Clock<=222000&&t.HTotal-t.HSyncEnd<=2047&&
        t.HSyncEnd-t.HSyncStart<=2047&&t.VTotal-t.VSyncEnd<=511&&
        t.VSyncEnd-t.VSyncStart<=31&&t.VSyncStart-t.VDisplay<=127;
}
enum NativeField {
    PvControl,PvVertical,PvEvenDelay,PvHa,PvHb,PvVa,PvVb,PvMux,PvPipe,
    PhyReset,PhyPower,Lane0,Lane1,Lane2,LaneClock,RefClock,PostDivider,VcoDivider,
    PllConfig,TmdsWord,Misc0,Misc1,Misc2,Misc3,Misc4,Misc5,Misc6,Misc7,Misc8,PllReset,PllPower,
    RmOffset,Fifo,PacketConfig,Scheduler,Ha,Hb,Va0,Vb0,Va1,Vb1,MiscControl,DeepColor,GcpConfig,GcpWord,Scrambler,
    ClockStop,VectorConfig,VectorCrossbar,CscControl,Csc11,Csc13,Csc21,Csc23,Csc31,Csc33,CscChannel,
    Avi0,Avi1,Avi2,Avi3,Avi4,Avi5,Avi6,Avi7,Avi8,VidControl,HvsControl1,NativeFieldCount
};
struct NativeRegister {uint32_t Region,Offset;};
// Region 6 here means this output's HDMI core. VID_CTL is at 0x48 for HDMI1.
static const NativeRegister NativeRegisters[]={
    {1,0},{1,4},{1,8},{1,0xc},{1,0x10},{1,0x14},{1,0x18},{1,0x34},{1,0x94},
    {4,0},{4,4},{4,8},{4,0xc},{4,0x10},{4,0x14},{4,0x1c},{4,0x28},{4,0x2c},
    {4,0x44},{4,0x54},{4,0x60},{4,0x64},{4,0x68},{4,0x6c},{4,0x70},{4,0x74},{4,0x78},{4,0x7c},{4,0x80},{4,0x190},{4,0x194},
    {5,0x18},{6,0x7c},{6,0xc4},{6,0xe8},{6,0xec},{6,0xf0},{6,0xf4},{6,0xf8},{6,0x100},{6,0x104},{6,0x114},{6,0x18c},{6,0x194},{6,0x198},{6,0x1e4},
    {9,0xbc},{9,0xf0},{9,0xf4},{11,0},{11,4},{11,8},{11,0xc},{11,0x10},{11,0x14},{11,0x18},{11,0x2c},
    {10,0x48},{10,0x4c},{10,0x50},{10,0x54},{10,0x58},{10,0x5c},{10,0x60},{10,0x64},{10,0x68},{8,0x44},{0,0x104}
};
static_assert(sizeof(NativeRegisters)/sizeof(*NativeRegisters)==NativeFieldCount,"register snapshot layout");
struct NativeState {uint32_t Value[NativeFieldCount];};
inline bool NativeBuild(const Timing&t,NativeState&s){
    if(!NativeSupported(t))return false;
    auto*v=s.Value;
    uint64_t bitRate=uint64_t(t.Clock)*10000;
    uint32_t lo=uint32_t((8000000000ull+bitRate-1)/bitRate),hi=uint32_t(11999999999ull/bitRate);
    if(lo>hi||hi>1023)return false;
    uint32_t div=lo+(hi-lo)/2;
    v[RmOffset]=0x80000000u|uint32_t((bitRate*div*(1ull<<21))/54000000);
    v[VcoDivider]=0x400|div;v[RefClock]=0x2036;v[PostDivider]=9;v[PllConfig]=0;
    v[PhyReset]=0x7f;v[PhyPower]=0x1cf;v[PllPower]=1;v[PllReset]|=1;
    v[Lane0]=v[Lane1]=v[Lane2]=v[LaneClock]=0x80828700;v[TmdsWord]=0;
    const uint32_t pll[]={0x810c6000,0x00b8c451,0x46402e31,0x00b8c005,0x42410261,0xcc021001,0xc8301c80,0xb0804444,0xf80f8000};
    for(uint32_t i=0;i<9;++i)v[Misc0+i]=pll[i];
    uint32_t hf=t.HSyncStart-t.HDisplay,hs=t.HSyncEnd-t.HSyncStart,hb=t.HTotal-t.HSyncEnd;
    uint32_t vf=t.VSyncStart-t.VDisplay,vs=t.VSyncEnd-t.VSyncStart,vb=t.VTotal-t.VSyncEnd;
    // Preserve the working firmware pixel pipeline (packing, FIFO threshold,
    // CSC bypass and underrun policy). A resolution change changes timings and
    // the PHY clock; it does not change the progressive 8-bit RGB wire format.
    uint32_t pixels=(v[PvVertical]&0x20000000)?1u:2u;
    v[PvHa]=((hb/pixels)<<16)|(hs/pixels);v[PvHb]=((hf/pixels)<<16)|(t.HDisplay/pixels);
    v[PvVa]=(vb<<16)|vs;v[PvVb]=(vf<<16)|t.VDisplay;
    v[Ha]=(hf<<16)|((t.Flags&3)<<14)|t.HDisplay;v[Hb]=(hb<<16)|hs;
    v[Va0]=v[Va1]=(vs<<24)|(vf<<16)|t.VDisplay;v[Vb0]=vb;
    // Progressive outputs have no second-field horizontal offset.
    v[Vb1]=vb;
    bool hdmi=!(t.Flags&0x200),limited=hdmi&&t.VideoId>1;
    v[Scheduler]=(v[Scheduler]&~3u)|(hdmi?1u:0u);
    // AVI packet: RGB, active-format same as picture, no overscan request,
    // quantization matching the CSC. Pack seven bytes into each two RAM words.
    uint8_t avi[35]={0x82,2,13,0,0x10,8,0,0,0};
    uint32_t aspect=(t.Flags>>4)&15;if(aspect==1||aspect==2)avi[5]|=uint8_t(aspect<<4);
    avi[6]=limited?4:8;avi[7]=uint8_t(t.VideoId<=127?t.VideoId:0);
    uint32_t sum=0;for(uint32_t i=0;i<17;++i)sum+=avi[i];avi[3]=uint8_t(0u-sum);
    for(uint32_t i=0;i<9;++i)v[Avi0+i]=0;
    for(uint32_t i=0,w=0;i<21;i+=7,w+=2){
        v[Avi0+w]=avi[i]|(uint32_t(avi[i+1])<<8)|(uint32_t(avi[i+2])<<16);
        v[Avi0+w+1]=avi[i+3]|(uint32_t(avi[i+4])<<8)|(uint32_t(avi[i+5])<<16)|(uint32_t(avi[i+6])<<24);
    }
    v[PacketConfig]=0x10000|(hdmi?4u:0u);
    v[VidControl]=(v[VidControl]&~0x18040000u)|0x80010000u|((t.Flags&1)?0u:0x08000000u)|((t.Flags&2)?0u:0x10000000u);
    return true;
}
}
