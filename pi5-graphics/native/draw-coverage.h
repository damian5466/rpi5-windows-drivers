#pragma once
#include "draw-bounds.h"
#include "abi.h"

namespace pi5 {
namespace coverage_detail {
using bounds_detail::Range;
struct Point {Range x,y;};
inline Range Difference(const Range&a,const Range&b){return {a.lo-b.hi,a.hi-b.lo,0,true,false};}
inline Range Product(const Range&a,const Range&b){double v[]={a.lo*b.lo,a.lo*b.hi,a.hi*b.lo,a.hi*b.hi};return {*std::min_element(v,v+4),*std::max_element(v,v+4),0,true,false};}
inline Range Area(const Point&a,const Point&b,const Point&c){
    auto x=Product(Difference(b.x,a.x),Difference(c.y,a.y)),y=Product(Difference(b.y,a.y),Difference(c.x,a.x));
    auto r=Difference(x,y);double error=(std::max(std::abs(x.lo),std::abs(x.hi))+std::max(std::abs(y.lo),std::abs(y.hi)))*0x1p-40+0x1p-30;
    r.lo-=error;r.hi+=error;return r;
}
inline Point Pixel(double x,double y){return {{x,x,0,true,false},{y,y,0,true,false}};}
}
// Return an interior rectangle whose pixel centres are all covered by an
// opaque triangle or a two-triangle quad. Shared vertices must have identical
// input words, so the common edge has identical GPU coordinates. Outward
// interval evaluation, projection/quantization margins, winding and clipping
// checks make an uncertain draw retain the ordinary tile loads.
inline bool DrawCoverage(const uint64_t*code,uint32_t words,const uint32_t*uniforms,uint32_t count,
                         const uint32_t*vertices,uint32_t vertexCount,uint32_t scalars,const float viewport[6],
                         uint32_t width,uint32_t height,const Pi5Pipeline&state,uint32_t rectangle[4]){
    using namespace coverage_detail;
    if(!code||!vertices||(vertexCount!=3&&vertexCount!=6)||!scalars||scalars>16||(count&&!uniforms)||
       (state.Flags&(1|PI5_PIPELINE_SHADER_BLEND))||state.ColorMask!=15||state.CullMode<1||state.CullMode>3)return false;
    uint32_t viewportWords[4],fixed[4];std::memcpy(viewportWords,viewport,16);
    if(!Pi5ViewportRect(viewportWords,width,height,fixed))return false;
    unsigned ids[6]={},unique[4]={},number=0;
    for(unsigned v=0;v<vertexCount;++v){unsigned i=0;
        for(;i<number;++i)if(!std::memcmp(vertices+size_t(v)*scalars,vertices+size_t(unique[i])*scalars,scalars*4))break;
        if(i==number){if(number==4)return false;unique[number++]=v;}ids[v]=i;
    }
    if(number!=(vertexCount==3?3u:4u))return false;
    Point points[4];
    for(unsigned v=0;v<number;++v){Range p[4];
        if(!bounds_detail::Position(code,words,uniforms,count,vertices+size_t(unique[v])*scalars,scalars,p)||p[3].lo<=1e-6)return false;
        if((state.Flags&4)&&(p[2].lo<0||p[2].hi>p[3].lo))return false;
        for(unsigned axis=0;axis<2;++axis){double q[]={p[axis].lo/p[3].lo,p[axis].lo/p[3].hi,p[axis].hi/p[3].lo,p[axis].hi/p[3].hi};
            double lo=*std::min_element(q,q+4),hi=*std::max_element(q,q+4),error=std::max(std::abs(lo),std::abs(hi))*0x1p-18+0x1p-18;lo-=error;hi+=error;
            if(axis){double old=lo;lo=-hi;hi=-old;}
            lo=viewport[axis]+(lo+1)*viewport[axis+2]*0.5-0.125;hi=viewport[axis]+(hi+1)*viewport[axis+2]*0.5+0.125;
            if(!std::isfinite(lo)||!std::isfinite(hi)||lo<-32768||hi>32768)return false;
            (axis?points[v].y:points[v].x)={lo,hi,0,true,false};
        }
    }
    for(unsigned v=0;v<vertexCount;v+=3){auto area=Area(points[ids[v]],points[ids[v+1]],points[ids[v+2]]);
        if(state.CullMode==1){if(area.lo<=0&&area.hi>=0)return false;}
        else{bool positive=(state.CullMode==3)!=bool(state.Flags&2);if(positive?area.lo<=0:area.hi>=0)return false;}
    }
    unsigned order[4]={0,1,2,3};
    double inner[4]={0,0,double(width),double(height)};
    if(number==4){
        std::sort(order,order+4,[&](unsigned a,unsigned b){return points[a].y.lo+points[a].y.hi<points[b].y.lo+points[b].y.hi;});
        auto left=[&](unsigned a,unsigned b){return points[a].x.lo+points[a].x.hi<points[b].x.lo+points[b].x.hi;};
        if(!left(order[0],order[1]))std::swap(order[0],order[1]);
        if(left(order[2],order[3]))std::swap(order[2],order[3]);
        unsigned shared[2]={},sharedCount=0;
        for(unsigned i=0;i<3;++i)for(unsigned j=3;j<6;++j)if(ids[i]==ids[j]){if(sharedCount==2)return false;shared[sharedCount++]=ids[i];}
        if(sharedCount!=2)return false;
        unsigned diagonal[2]={};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<2;++j)if(order[i]==shared[j])diagonal[j]=i;
        if((diagonal[0]+2)%4!=diagonal[1])return false;
        for(unsigned i=0;i<4;++i)if(Area(points[order[i]],points[order[(i+1)%4]],points[order[(i+2)%4]]).lo<=0)return false;
        inner[0]=std::max(points[order[0]].x.hi,points[order[3]].x.hi);
        inner[1]=std::max(points[order[0]].y.hi,points[order[1]].y.hi);
        inner[2]=std::min(points[order[1]].x.lo,points[order[2]].x.lo);
        inner[3]=std::min(points[order[2]].y.lo,points[order[3]].y.lo);
    }else if(Area(points[0],points[1],points[2]).hi<0)std::swap(order[1],order[2]);
    for(unsigned axis=0;axis<2;++axis){double limit=axis?height:width;
        rectangle[axis]=uint32_t(std::max(0.0,std::min(limit,std::ceil(inner[axis]-0.5))));
        rectangle[axis+2]=uint32_t(std::max(0.0,std::min(limit,std::floor(inner[axis+2]-0.5)+1)));
        rectangle[axis]=std::max(rectangle[axis],(fixed[axis]+32767)>>16);
        rectangle[axis+2]=std::min(rectangle[axis+2],(fixed[axis]+fixed[axis+2]+32767)>>16);
        if(state.Flags&8){rectangle[axis]=std::max(rectangle[axis],state.Scissor[axis]);rectangle[axis+2]=std::min(rectangle[axis+2],state.Scissor[axis+2]);}
        if(rectangle[axis]>=rectangle[axis+2])return false;
    }
    for(unsigned corner=0;corner<4;++corner){auto p=Pixel((corner&1?rectangle[2]-1:rectangle[0])+0.5,(corner&2?rectangle[3]-1:rectangle[1])+0.5);
        for(unsigned edge=0;edge<number;++edge)if(Area(points[order[edge]],points[order[(edge+1)%number]],p).lo<=0)return false;
    }
    return true;
}
}
