#include "texture.h"
#include <string.h>
namespace pi5 {
uint32_t TextureBytes(uint32_t width,uint32_t height){if(!width||!height||width>4096||height>4096)return 0;return ((width+31)&~31u)*((height+7)&~7u)*4;}
bool EncodeTexture(uint32_t width,uint32_t height,uint32_t pitch,const void *source,void *destination,uint32_t capacity,uint32_t &bytes){
    bytes=TextureBytes(width,height);if(!bytes||bytes>capacity||!source||!destination||pitch<width*4||pitch>16384||(pitch&3)||((reinterpret_cast<uintptr_t>(source)|reinterpret_cast<uintptr_t>(destination))&3))return false;
    memset(destination,0,bytes);auto out=static_cast<uint32_t*>(destination);auto in=static_cast<const volatile uint32_t*>(source);uint32_t rows=(height+7)/8;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
        // UIF without XOR: four 8x8 blocks per column; each block contains four 4x4 utiles.
        uint32_t block=(x/32)*rows*4+(y/8)*4+(x/8)%4;
        uint32_t offset=block*64+((y/4)%2)*32+((x/4)%2)*16+(y%4)*4+x%4;
        out[offset]=in[y*(pitch/4)+x];
    }return true;
}
static void Bits(void *data,unsigned start,unsigned count,uint32_t value){auto p=static_cast<uint8_t*>(data);for(unsigned bit=0;bit<count;++bit)if(value&(1u<<bit))p[(start+bit)/8]|=uint8_t(1u<<((start+bit)%8));}
bool TextureDescriptors(uint32_t width,uint32_t height,uint32_t address,bool bgra,bool linear,void *texture,void *sampler,bool opaque,bool constants,uint32_t borderMask,const uint32_t *border){
    uint32_t bytes=TextureBytes(width,height);if(!bytes||!address||(address&4095)||!texture||!sampler||uint64_t(address)+bytes>UINT64_C(0x100000000))return false;
    if(borderMask>3||(borderMask&&!border)||(constants&&(bgra||linear||height!=1||borderMask)))return false;
    if(borderMask)for(unsigned i=0;i<4;++i)if(border[i]>0x3f800000u)return false;
    memset(texture,0,24);memset(sampler,0,24);Bits(texture,0,32,address);Bits(texture,33,24,bytes/64);Bits(texture,58,14,width);Bits(texture,72,14,height);Bits(texture,86,14,1);Bits(texture,100,7,constants?29:4);
    Bits(texture,107,1,1);Bits(texture,108,3,bgra?4:2);Bits(texture,111,3,constants?0:3);Bits(texture,114,3,constants?0:bgra?2:4);Bits(texture,117,3,opaque||constants?1:5);Bits(texture,134,1,1);Bits(texture,135,1,1);
    Bits(sampler,0,1,!linear);Bits(sampler,1,1,!linear);Bits(sampler,2,1,1);Bits(sampler,48,3,constants||(borderMask&1)?3:1);Bits(sampler,51,3,constants||(borderMask&2)?3:1);Bits(sampler,54,3,constants?3:1);
    // V3D 7.1 consumes IEEE float border components before texture swizzling.
    if(borderMask){Bits(sampler,58,3,7);for(unsigned i=0;i<4;++i)Bits(sampler,64+i*32,32,border[bgra&&i%2==0?2-i:i]);}
    return true;
}
namespace {
constexpr uint32_t PageBytes=4096,PageCacheRows=32,PageRowsTimes15=6;
uint32_t Align(uint32_t value,uint32_t alignment){return (value+alignment-1)&~(alignment-1);}
uint32_t Minify(uint32_t value,uint32_t level){value>>=level;return value?value:1;}
uint32_t NextPowerOfTwo(uint32_t value){uint32_t p=1;while(p<value)p<<=1;return p;}
// UIF-block row padding the TMU applies to non-base UIF levels (32bpp, 8-row blocks).
uint32_t UifPad(uint32_t paddedHeight){
    uint32_t rows=paddedHeight/8,offset=rows%PageCacheRows;
    if(!offset)return 0;
    if(offset<PageRowsTimes15)return rows<PageCacheRows?0:PageRowsTimes15-offset;
    if(offset>PageCacheRows-PageRowsTimes15)return PageCacheRows-offset;
    return 0;
}
}
bool LayoutTextureChain(uint32_t width,uint32_t height,uint32_t levels,TextureChain &out){
    out={};if(!width||!height||width>4096||height>4096||!levels||levels>PI5_MAX_LEVELS)return false;
    uint32_t largest=width>height?width:height,possible=1;while(largest>>possible)++possible;if(levels>possible)return false;
    uint32_t potWidth=2*NextPowerOfTwo(Minify(width,1)),potHeight=2*NextPowerOfTwo(Minify(height,1)),below=0,sizes[PI5_MAX_LEVELS]={};
    for(uint32_t i=levels;i-->0;){
        auto &l=out.level[i];l.width=Minify(width,i);l.height=Minify(height,i);
        uint32_t w=i<2?l.width:Minify(potWidth,i),h=i<2?l.height:Minify(potHeight,i);
        if(i&&(w<=4||h<=4)){l.tiling=Tiling::Linear;w=Align(w,4);h=Align(h,4);}
        else if(i&&w<=8){l.tiling=Tiling::UbLinear1;w=Align(w,8);h=Align(h,8);}
        else if(i&&w<=16){l.tiling=Tiling::UbLinear2;w=Align(w,16);h=Align(h,8);}
        else{l.tiling=Tiling::Uif;w=Align(w,32);h=Align(h,8);if(i){h+=UifPad(h)*8;l.xorEnabled=(h/8)%PageCacheRows==0;}}
        l.paddedWidth=w;l.paddedHeight=h;uint32_t size=w*h*4;
        // The TMU page-aligns level 1 when it or a smaller level could be UIF XOR.
        if(i==1&&w>32&&h>(PageCacheRows-PageRowsTimes15)*8)size=Align(size,PageBytes);
        sizes[i]=size;if(i)below+=size;
    }
    // Level 0 is page aligned; level 1 ends where level 0 starts, and so on down the chain.
    out.base=Align(below,PageBytes);out.bytes=out.base+sizes[0];out.level[0].offset=out.base;
    for(uint32_t i=1,above=0;i<levels;++i){above+=sizes[i];out.level[i].offset=out.base-above;}
    out.levels=levels;return true;
}
uint32_t TexelOffset(const TextureLevel &l,uint32_t x,uint32_t y){
    uint32_t utile=(y%4)*16+(x%4)*4;
    if(l.tiling==Tiling::Linear)return 64*(x/4+y/4)+utile;
    uint32_t inBlock=((x&4)?64u:0u)+((y&4)?128u:0u)+utile;
    if(l.tiling!=Tiling::Uif)return 256*((y/8)*(l.tiling==Tiling::UbLinear1?1:2)+x/8)+inBlock;
    uint32_t bx=x/8,by=y/8;if(l.xorEnabled&&((bx/4)&1))by^=0x10;
    return ((bx/4)*((l.paddedHeight/8-1)*4)+bx+by*4)*256+inBlock;
}
bool EncodeTextureLevel(const TextureChain &chain,uint32_t level,const void *source,uint32_t pitch,void *destination,uint32_t capacity){
    if(level>=chain.levels||!source||!destination||chain.bytes>capacity||((reinterpret_cast<uintptr_t>(source)|reinterpret_cast<uintptr_t>(destination)|pitch)&3))return false;
    const auto &l=chain.level[level];if(pitch<l.width*4||uint64_t(l.offset)+uint64_t(l.paddedWidth)*l.paddedHeight*4>chain.bytes)return false;
    auto out=static_cast<uint8_t*>(destination)+l.offset;auto in=static_cast<const volatile uint32_t*>(source);
    for(uint32_t y=0;y<l.height;++y)for(uint32_t x=0;x<l.width;++x){uint32_t value=in[size_t(y)*(pitch/4)+x];memcpy(out+TexelOffset(l,x,y),&value,4);}
    return true;
}
bool ChainDescriptor(const TextureChain &chain,uint32_t address,bool bgra,bool opaque,void *texture){
    if(!chain.levels||!address||(address&4095)||!texture||uint64_t(address)+chain.bytes>UINT64_C(0x100000000))return false;
    memset(texture,0,24);Bits(texture,0,32,address+chain.base);Bits(texture,33,24,Align(chain.bytes,64)/64);Bits(texture,58,14,chain.level[0].width);Bits(texture,72,14,chain.level[0].height);Bits(texture,86,14,1);Bits(texture,100,7,4);
    Bits(texture,107,1,1);Bits(texture,108,3,bgra?4:2);Bits(texture,111,3,3);Bits(texture,114,3,bgra?2:4);Bits(texture,117,3,opaque?1:5);Bits(texture,120,4,chain.levels-1);Bits(texture,134,1,1);
    // Single-level textures keep the verified XOR-disabled configuration.
    if(chain.levels==1)Bits(texture,135,1,1);
    return true;
}
bool SamplerDescriptor(uint32_t filter,uint32_t addressModes,const uint32_t *border,uint32_t minLod,uint32_t maxLod,bool bgra,void *sampler){
    bool usesBorder=Pi5UsesBorder(addressModes);
    if(!sampler||filter>7||!Pi5ValidAddressModes(addressModes)||(usesBorder&&!border)||minLod>maxLod||maxLod>(15u<<8))return false;
    if(usesBorder)for(unsigned i=0;i<4;++i)if(border[i]>0x3f800000u)return false;
    memset(sampler,0,24);Bits(sampler,0,1,!(filter&PI5_FILTER_MAG_LINEAR));Bits(sampler,1,1,!(filter&PI5_FILTER_MIN_LINEAR));Bits(sampler,2,1,!(filter&PI5_FILTER_MIP_LINEAR));
    static const uint32_t hardwareMode[]={1,3,0,2,4};
    Bits(sampler,8,12,minLod);Bits(sampler,20,12,maxLod);Bits(sampler,48,3,hardwareMode[addressModes&7]);Bits(sampler,51,3,hardwareMode[addressModes>>3]);Bits(sampler,54,3,1);
    if(usesBorder){Bits(sampler,58,3,7);for(unsigned i=0;i<4;++i)Bits(sampler,64+i*32,32,border[bgra&&i%2==0?2-i:i]);}
    return true;
}
bool BufferDescriptors(uint32_t elements,uint32_t kind,uint32_t address,void *texture,void *sampler){
    uint32_t elementBytes=Pi5BufferElementBytes(kind);
    if(!elements||elements>PI5_MAX_BYTE_ELEMENTS||!elementBytes||!address||(address&63)||!texture||!sampler||uint64_t(address)+uint64_t(elements)*elementBytes>UINT64_C(0x100000000))return false;
    memset(texture,0,24);memset(sampler,0,24);Bits(texture,0,32,address);Bits(texture,58,14,elements&16383);Bits(texture,72,14,elements>>14);Bits(texture,86,14,1);Bits(texture,100,7,(Pi5BufferComponentBytes(kind)==1?96:Pi5BufferComponentBytes(kind)==2?102:108)+(Pi5BufferComponents(kind)==1?0:Pi5BufferComponents(kind)==2?2:4)+(Pi5BufferSigned(kind)?0:1));
    for(unsigned c=0;c<4;++c)Bits(texture,108+c*3,3,c<Pi5BufferComponents(kind)?c+2:c==3?1:0);
    Bits(sampler,0,1,1);Bits(sampler,1,1,1);Bits(sampler,2,1,1);Bits(sampler,48,3,1);Bits(sampler,51,3,1);Bits(sampler,54,3,1);
    return true;
}
}
