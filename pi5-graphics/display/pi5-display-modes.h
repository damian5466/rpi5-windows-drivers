/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>
#include "pi5-mailbox.h"

namespace pi5display {
using Timing=PI5_MAILBOX_DISPLAY_TIMING;
static const uint32_t MaxModes=128,MaxEdidBlocks=8;
#include "pi5-edid-tables.inc"

inline uint32_t Pitch(const Timing&t){return (uint32_t(t.HDisplay)*4+63)&~63u;}
inline uint32_t FrameBytes(const Timing&t){return (Pitch(t)*t.VDisplay+4095)&~4095u;}
inline bool Supported(const Timing&t){
    // Raster RGB scanout only. The two fallback surfaces also have to fit the
    // provider's 64 MiB mapping; direct primaries retain their own allocation.
    return !((t.HDisplay|t.HSyncStart|t.HSyncEnd|t.HTotal)&1)&&t.Clock>=25000&&t.Clock<=600000&&t.HDisplay>=640&&t.HDisplay<=4096&&
        t.VDisplay>=480&&t.VDisplay<=2160&&t.HDisplay<=t.HSyncStart&&t.HSyncStart<t.HSyncEnd&&
        t.HSyncEnd<t.HTotal&&t.HTotal<=8192&&t.VDisplay<=t.VSyncStart&&t.VSyncStart<t.VSyncEnd&&
        t.VSyncEnd<t.VTotal&&t.VTotal<=8192&&!t.HSkew&&t.VScan<=1&&t.VRefresh>=20&&t.VRefresh<=240&&
        !(t.Flags&~0x3f3u)&&((t.Flags>>4)&15)<=4&&FrameBytes(t)<=32u*1024*1024;
}
inline bool Same(const Timing&a,const Timing&b){
    return a.Clock==b.Clock&&a.HDisplay==b.HDisplay&&a.HSyncStart==b.HSyncStart&&
        a.HSyncEnd==b.HSyncEnd&&a.HTotal==b.HTotal&&a.VDisplay==b.VDisplay&&
        a.VSyncStart==b.VSyncStart&&a.VSyncEnd==b.VSyncEnd&&a.VTotal==b.VTotal&&
        (a.Flags&7)==(b.Flags&7);
}
inline bool Checksum(const uint8_t*b){uint32_t s=0;for(uint32_t i=0;i<128;++i)s+=b[i];return !(s&255);}
inline bool Header(const uint8_t*b){static const uint8_t h[]={0,255,255,255,255,255,255,0};for(uint32_t i=0;i<8;++i)if(b[i]!=h[i])return false;return b[18]==1;}
inline uint16_t Word(const uint8_t*b){return uint16_t(b[0]|(uint16_t(b[1])<<8));}
inline Timing Detailed(const uint8_t*b){
    Timing t={};
    if(!Word(b)||(b[17]&0xf9)!=0x18)return t; // progressive digital separate sync
    t.Clock=uint32_t(Word(b))*10;
    t.HDisplay=uint16_t(b[2]|((b[4]&0xf0)<<4));t.HTotal=uint16_t(t.HDisplay+b[3]+((b[4]&15)<<8));
    t.VDisplay=uint16_t(b[5]|((b[7]&0xf0)<<4));t.VTotal=uint16_t(t.VDisplay+b[6]+((b[7]&15)<<8));
    t.HSyncStart=uint16_t(t.HDisplay+b[8]+((b[11]&0xc0)<<2));
    t.HSyncEnd=uint16_t(t.HSyncStart+b[9]+((b[11]&0x30)<<4));
    t.VSyncStart=uint16_t(t.VDisplay+(b[10]>>4)+((b[11]&12)<<2));
    t.VSyncEnd=uint16_t(t.VSyncStart+(b[10]&15)+((b[11]&3)<<4));
    t.Flags=((b[17]&2)?1u:0u)|((b[17]&4)?2u:0u);
    if(t.HTotal&&t.VTotal)t.VRefresh=uint16_t((uint64_t(t.Clock)*1000+uint32_t(t.HTotal)*t.VTotal/2)/(uint32_t(t.HTotal)*t.VTotal));
    return t;
}
struct Monitor {
    uint8_t Edid[128*MaxEdidBlocks];
    uint32_t Bytes,Count,Preferred;
    bool Hdmi;
    Timing Modes[MaxModes];
    bool Add(Timing t,bool preferred=false){
        if(!Supported(t))return false;
        uint32_t i=0;for(;i<Count;++i)if(Same(t,Modes[i]))break;
        if(i==Count){if(Count==MaxModes)return false;Modes[Count++]=t;}
        else if(t.VideoId&&!Modes[i].VideoId){Modes[i].VideoId=t.VideoId;Modes[i].Flags|=t.Flags&0xf0;}
        if(preferred&&Preferred==MaxModes)Preferred=i;
        return true;
    }
    void Standard(const uint8_t*p,uint8_t revision){
        if(p[0]<=1)return;
        uint32_t w=(uint32_t(p[0])+31)*8,h,rate=(p[1]&63)+60;
        switch(p[1]>>6){case 0:h=revision<3?w:w*10/16;break;case 1:h=w*3/4;break;case 2:h=w*4/5;break;default:h=w*9/16;break;}
        const Timing*found=nullptr;
        for(const auto&t:DmtModes)if(t.HDisplay==w&&t.VDisplay==h&&t.VRefresh==rate&&Supported(t)){
            if(!found)found=&t;
            if(t.HTotal-t.HDisplay!=160){found=&t;break;}
        }
        if(found)Add(*found);
    }
    bool Parse(){
        Count=0;Preferred=MaxModes;Hdmi=false;
        if(Bytes<128||Bytes>sizeof(Edid)||(Bytes&127)||!Header(Edid)||!Checksum(Edid))return false;
        // The preferred base detailed timing precedes CTA/native alternatives.
        for(uint32_t i=54;i<=108;i+=18){
            if(Word(Edid+i))Add(Detailed(Edid+i),i==54);
            else if(Edid[i+3]==0xfa)for(uint32_t j=i+5;j<i+17;j+=2)Standard(Edid+j,Edid[19]);
        }
        uint32_t established=Edid[35]|(uint32_t(Edid[36])<<8)|((uint32_t(Edid[37])&128)<<9);
        for(uint32_t i=0;i<17;++i)if(established&(1u<<i))Add(EstablishedModes[i]);
        for(uint32_t i=38;i<54;i+=2)Standard(Edid+i,Edid[19]);
        uint32_t blocks=Bytes/128;if(blocks>uint32_t(Edid[126])+1)blocks=uint32_t(Edid[126])+1;
        for(uint32_t block=1;block<blocks;++block){
            const uint8_t*b=Edid+block*128;
            if(!Checksum(b)||b[0]!=2||b[1]<3||b[2]<4||b[2]>127)continue;
            uint32_t end=b[2];
            for(uint32_t at=4;at<end;){
                uint32_t n=b[at]&31,tag=b[at]>>5;++at;
                if(n>end-at)break;
                if(tag==2)for(uint32_t j=0;j<n;++j){
                    uint32_t svd=b[at+j],vic=svd>=129&&svd<=192?svd&127:svd;
                    if(vic&&vic<=sizeof(CtaModes)/sizeof(CtaModes[0]))Add(CtaModes[vic-1],svd>=129&&svd<=192);
                }
                if(tag==3&&n>=3){uint32_t oui=b[at]|(uint32_t(b[at+1])<<8)|(uint32_t(b[at+2])<<16);if(oui==0xc03||oui==0xc45dd8)Hdmi=true;}
                // YCbCr 4:2:0-only modes and unsupported extended blocks are
                // intentionally not advertised as RGB timings.
                at+=n;
            }
            uint32_t native=b[3]&15;
            for(uint32_t at=end,index=0;at+18<=127;at+=18,++index)Add(Detailed(b+at),index<native);
        }
        for(uint32_t i=0;i<Count;++i)if(!Hdmi)Modes[i].Flags|=0x200;
        if(Count&&Preferred==MaxModes)Preferred=0;
        return Count!=0;
    }
};
}
