#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace pi5 {
// Optional conservative bounds for small draws. Interpret only the affine
// prefix of our coordinate program, through its four clip-position stores.
// Unrecognized instructions, nonfinite values and eye-plane crossings leave
// the original draw untouched. Rendering still executes the GPU programs.
namespace bounds_detail {
constexpr uint64_t Nop=UINT64_C(0x38003186bb03f000);
inline uint64_t Add(unsigned op,unsigned d,unsigned a,unsigned b){
    return (Nop&~((UINT64_C(1)<<44)|(UINT64_C(63)<<32)|(UINT64_C(255)<<24)|UINT64_C(4095)))|
        (uint64_t(d)<<32)|(uint64_t(op)<<24)|(uint64_t(a)<<6)|b;
}
inline uint64_t Mul(unsigned d,unsigned a,unsigned b){
    return (Nop&~((UINT64_C(63)<<58)|(UINT64_C(1)<<45)|(UINT64_C(63)<<38)|(UINT64_C(4095)<<12)))|
        (UINT64_C(21)<<58)|(uint64_t(d)<<38)|(uint64_t(a)<<18)|(uint64_t(b)<<12);
}
struct Range {
    double lo=0,hi=0;
    uint32_t bits=0;
    bool valid=false,exact=false;
};
inline Range Word(uint32_t bits){float f;std::memcpy(&f,&bits,4);return {double(f),double(f),bits,true,true};}
inline Range Immediate(unsigned i){return i<48?Word(i<16?i:i<32?0xfffffff0u+(i-16):0x3b800000u+((i-32)<<23)):Range{};}
inline bool Finite(const Range&r){return r.valid&&std::isfinite(r.lo)&&std::isfinite(r.hi);}
inline Range Rounded(double lo,double hi){
    // A generous single-precision error envelope also includes denormal
    // flushing. Double arithmetic prevents the bounds calculation from
    // rounding inward at the precision used by the QPU.
    double magnitude=std::max(std::abs(lo),std::abs(hi));
    if(!std::isfinite(magnitude)||magnitude>double(std::numeric_limits<float>::max())*0.5)return {};
    double error=magnitude*0x1p-21+double(std::numeric_limits<float>::min());
    return {lo-error,hi+error,0,true,false};
}
inline bool Position(const uint64_t*code,uint32_t words,const uint32_t*uniforms,uint32_t count,
                     const uint32_t*vertex,uint32_t scalars,Range position[4],const Range*attributes=nullptr){
    Range rf[32];uint32_t used=0,written=0;
    for(uint32_t i=0;i<words&&i<512;++i){uint64_t word=code[i];
        if(word==Nop)continue;
        if((word&~(UINT64_C(31)<<46))==UINT64_C(0x39803186bb03f000)){
            if(used==count)return false;rf[(word>>46)&31]=Word(uniforms[used++]);continue;
        }
        if((word&~((UINT64_C(31)<<32)|(UINT64_C(15)<<6)))==UINT64_C(0x39c02180bc03f000)){
            unsigned index=unsigned((word>>6)&15);if(index>=scalars)return false;rf[(word>>32)&31]=attributes?attributes[index]:Word(vertex[index]);continue;
        }
        if((word&~UINT64_C(4095))==UINT64_C(0x39c02180be03f000)){
            unsigned index=unsigned((word>>6)&63),source=unsigned(word&63);if(source>=32)return false;
            if(index<4){if(!Finite(rf[source]))return false;position[index]=rf[source];written|=1u<<index;if(written==15)return true;}
            continue;
        }
        unsigned op=unsigned((word>>24)&255),dst=unsigned((word>>32)&63),a=unsigned((word>>6)&63),b=unsigned(word&63);
        if(dst<32&&a<48&&word==(Add(249,dst,a,3)|(UINT64_C(14)<<53))){
            rf[dst]=Immediate(a);continue;
        }
        unsigned signal=unsigned((word>>53)&31);
        if(dst<32&&(signal==0||signal==14||signal==15)&&word==(Add(op,dst,a,b)|(uint64_t(signal)<<53))){
            Range x=signal==14?Immediate(a):a<32?rf[a]:Range{};if(!x.valid)return false;
            if(!signal&&op==246&&(b==32||b==36)&&x.exact){double value=b==32?double(int32_t(x.bits)):double(x.bits);rf[dst]=Rounded(value,value);continue;}
            Range y=signal==15?Immediate(b):b<32?rf[b]:Range{};if(!y.valid)return false;
            if((op==181||op==182||op==183)&&x.exact&&y.exact){rf[dst]=Word(op==181?x.bits&y.bits:op==182?x.bits|y.bits:x.bits^y.bits);continue;}
            if((op==181||op==183)&&x.exact)std::swap(x,y);
            if(op==183&&y.exact&&y.bits==0x80000000u&&Finite(x)){rf[dst]={-x.hi,-x.lo,0,true,false};continue;}
            if(op==181&&y.exact&&y.bits==0x7fffffffu&&Finite(x)){rf[dst]={x.lo<=0&&x.hi>=0?0:std::min(std::abs(x.lo),std::abs(x.hi)),std::max(std::abs(x.lo),std::abs(x.hi)),0,true,false};continue;}
            if((op!=5&&op!=69)||!Finite(x)||!Finite(y))return false;
            // Zero arithmetic is exact even at the near clip plane. Keep its
            // numeric interval without asserting a particular signed-zero bit.
            if(x.lo==0&&x.hi==0&&y.lo==0&&y.hi==0){rf[dst]={0,0,0,true,false};continue;}
            rf[dst]=op==69?Rounded(x.lo-y.hi,x.hi-y.lo):Rounded(x.lo+y.lo,x.hi+y.hi);continue;
        }
        dst=unsigned((word>>38)&63);a=unsigned((word>>18)&63);b=unsigned((word>>12)&63);
        if(dst>=32||(signal!=0&&signal!=30&&signal!=31)||word!=(Mul(dst,a,b)|(uint64_t(signal)<<53)))return false;
        const auto x=signal==30?Immediate(a):a<32?rf[a]:Range{},y=signal==31?Immediate(b):b<32?rf[b]:Range{};
        if(!Finite(x)||!Finite(y))return false;
        if((x.lo==0&&x.hi==0)||(y.lo==0&&y.hi==0)){rf[dst]={0,0,0,true,false};continue;}
        double v0=x.lo*y.lo,v1=x.lo*y.hi,v2=x.hi*y.lo,v3=x.hi*y.hi;
        rf[dst]=Rounded(std::min(std::min(v0,v1),std::min(v2,v3)),
                        std::max(std::max(v0,v1),std::max(v2,v3)));
    }
    return false;
}
}
inline bool DrawBounds(const uint64_t*code,uint32_t words,const uint32_t*uniforms,uint32_t count,
                       const uint32_t*vertices,uint32_t vertexCount,uint32_t scalars,const float viewport[6],
                       uint32_t width,uint32_t height,uint32_t rectangle[4]){
    if(!code||!vertices||!vertexCount||vertexCount>4095||!scalars||scalars>16||(count&&!uniforms))return false;
    // Large shell meshes use one interval per attribute, then interpret the
    // affine coordinate prefix once. This encloses all vertices without a
    // shader interpretation for every tessellated triangle. Unsupported bit
    // conversions or an uncertain eye-plane crossing retain the full bounds.
    bounds_detail::Range attributes[16];bool aggregate=vertexCount>128;
    if(aggregate)for(uint32_t scalar=0;scalar<scalars;++scalar){
        auto&range=attributes[scalar];range=bounds_detail::Word(vertices[scalar]);
        for(uint32_t vertex=1;vertex<vertexCount;++vertex){auto value=bounds_detail::Word(vertices[size_t(vertex)*scalars+scalar]);
            if(!bounds_detail::Finite(range)||!bounds_detail::Finite(value)){range={};break;}
            range.lo=std::min(range.lo,value.lo);range.hi=std::max(range.hi,value.hi);range.exact=range.exact&&range.bits==value.bits;
        }
    }
    double lower[2]={double(width),double(height)},upper[2]={0,0};
    for(uint32_t vertex=0;vertex<(aggregate?1u:vertexCount);++vertex){bounds_detail::Range p[4];
        if(!bounds_detail::Position(code,words,uniforms,count,vertices+size_t(vertex)*scalars,scalars,p,aggregate?attributes:nullptr)||p[3].lo<=1e-6)return false;
        for(unsigned axis=0;axis<2;++axis){
            double v0=p[axis].lo/p[3].lo,v1=p[axis].lo/p[3].hi,v2=p[axis].hi/p[3].lo,v3=p[axis].hi/p[3].hi;
            double lo=std::min(std::min(v0,v1),std::min(v2,v3)),hi=std::max(std::max(v0,v1),std::max(v2,v3));
            // Cover reciprocal approximation, projection rounding, fixed-point
            // raster coordinates and pixel-centre conventions as well.
            double error=std::max(std::abs(lo),std::abs(hi))*0x1p-18+0x1p-18;lo-=error;hi+=error;
            if(axis){double oldLo=lo;lo=-hi;hi=-oldLo;}
            lo=viewport[axis]+(lo+1)*viewport[axis+2]*0.5-2;
            hi=viewport[axis]+(hi+1)*viewport[axis+2]*0.5+2;
            if(!std::isfinite(lo)||!std::isfinite(hi))return false;
            lower[axis]=std::min(lower[axis],lo);upper[axis]=std::max(upper[axis],hi);
        }
    }
    for(unsigned axis=0;axis<2;++axis){double limit=axis?height:width;
        rectangle[axis]=uint32_t(std::max(0.0,std::min(limit,std::floor(lower[axis]))));
        rectangle[axis+2]=uint32_t(std::max(0.0,std::min(limit,std::ceil(upper[axis]))));
    }
    return rectangle[0]<rectangle[2]&&rectangle[1]<rectangle[3];
}
}
