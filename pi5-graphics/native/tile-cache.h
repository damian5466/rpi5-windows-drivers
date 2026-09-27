#pragma once
#include <stdint.h>
namespace pi5 {
// Entries describe copies in the fixed TFU arena, never the source allocation.
// Callers only retain sources that cannot be mapped for CPU writes, and notify
// every GPU, present and paging write to the native segment.
struct TileCache {
    struct Entry {uint64_t identity,source;uint32_t sourceBytes,first,levels,arena,bytes;};
    Entry entries[128]={};
    unsigned next=0;
    static bool Overlap(uint64_t a,uint64_t bytes,uint64_t b,uint64_t other){return bytes&&other&&(a<=b?b-a<bytes:a-b<other);}
    bool Find(uint64_t identity,uint64_t source,uint32_t first,uint32_t levels,uint32_t arena,uint32_t bytes) const {
        if(!identity)return false;
        for(const auto&e:entries)if(e.identity==identity&&e.source==source&&e.first==first&&e.levels==levels&&e.arena==arena&&e.bytes==bytes)return true;
        return false;
    }
    bool Locate(uint64_t identity,uint64_t source,uint32_t first,uint32_t levels,uint32_t bytes,uint32_t &arena) const {
        if(!identity)return false;
        for(const auto&e:entries)if(e.identity==identity&&e.source==source&&e.first==first&&e.levels==levels&&e.bytes==bytes){arena=e.arena;return true;}
        return false;
    }
    void WriteSource(uint64_t offset,uint64_t bytes){for(auto&e:entries)if(e.identity&&Overlap(offset,bytes,e.source,e.sourceBytes))e.identity=0;}
    void WriteArena(uint32_t offset,uint32_t bytes){for(auto&e:entries)if(e.identity&&Overlap(offset,bytes,e.arena,e.bytes))e.identity=0;}
    void Store(uint64_t identity,uint64_t source,uint32_t sourceBytes,uint32_t first,uint32_t levels,uint32_t arena,uint32_t bytes){
        if(!identity)return;
        Entry value={identity,source,sourceBytes,first,levels,arena,bytes};
        for(auto&e:entries)if(!e.identity){e=value;return;}
        entries[next]=value;next=(next+1)%128;
    }
};
// Views remain pinned until the complete bin/render job retires. Allocation
// walks forward from the persistent cursor, wrapping only around those pins.
// Cache entries themselves may be evicted; live descriptors must never be.
struct TextureView {
    uint64_t identity,source;
    uint32_t kind,first,count,arena,bytes;
};
inline bool SameTextureView(const TextureView&a,const TextureView&b){return a.identity==b.identity&&a.source==b.source&&a.kind==b.kind&&a.first==b.first&&a.count==b.count&&a.bytes==b.bytes;}
inline bool PlaceTextureView(const TileCache&cache,uint32_t capacity,uint32_t &cursor,TextureView &view,bool cacheable,
                             const TextureView*live,uint32_t count,const TextureView*extra=nullptr,uint32_t extras=0){
    for(uint32_t i=0;i<count+extras;++i){const auto&v=i<count?live[i]:extra[i-count];if(SameTextureView(v,view)){view.arena=v.arena;return true;}}
    uint32_t reserved=(view.bytes+4095)&~4095u;
    if(!view.bytes||view.bytes>capacity||reserved<view.bytes||reserved>capacity)return false;
    auto Overlaps=[&](uint32_t at){for(uint32_t i=0;i<count+extras;++i){const auto&v=i<count?live[i]:extra[i-count];if(TileCache::Overlap(at,reserved,v.arena,(v.bytes+4095)&~4095u))return true;}return false;};
    uint32_t cached=0;
    if(cacheable&&cache.Locate(view.identity,view.source,view.first,view.count,view.bytes,cached)&&cached<=capacity-reserved&&!Overlaps(cached)){view.arena=cached;return true;}
    for(unsigned pass=0;pass<2;++pass){uint32_t at=pass?0:cursor;
        for(uint32_t step=0;step<=count+extras&&at<=capacity-reserved;++step){uint32_t next=at;
            for(uint32_t i=0;i<count+extras;++i){const auto&v=i<count?live[i]:extra[i-count];if(TileCache::Overlap(at,reserved,v.arena,(v.bytes+4095)&~4095u)){uint32_t end=v.arena+((v.bytes+4095)&~4095u);if(end>next)next=end;}}
            if(next==at){view.arena=at;cursor=at+reserved;return true;}at=next;
        }
    }
    return false;
}
}
