// Only non-CPU-visible, single-level render targets enter this
// cache. The worker serializes access; destruction takes its GPU mutex too.
uint32_t GpuPath::FindShadow(uint64_t identity,uint64_t offset){
    for(uint32_t i=0;i<RTL_NUMBER_OF(shadows);++i){auto&e=shadows[i];if(e.identity&&e.source==offset&&(!identity||e.identity==identity)){e.age=++shadowAge;return i;}}
    return UINT32_MAX;
}
NTSTATUS GpuPath::ResolveShadow(uint32_t index){
    if(index>=RTL_NUMBER_OF(shadows))return STATUS_INVALID_PARAMETER;auto&e=shadows[index];if(!e.identity||!e.dirty)return STATUS_SUCCESS;
    TRY(CopySurface(target.Address+e.arena,segment.Address+static_cast<ULONG>(e.source),e.info.Width,e.info.Height,e.info.Pitch,e.info.Pitch,e.rows,0));
    KeMemoryBarrier();e.dirty=false;return STATUS_SUCCESS;
}
NTSTATUS GpuPath::SyncShadows(uint64_t offset,uint64_t bytes,bool invalidate){
    for(uint32_t i=0;i<RTL_NUMBER_OF(shadows);++i){auto&e=shadows[i];if(e.identity&&pi5::TileCache::Overlap(offset,bytes,e.source,e.info.Bytes)){TRY(ResolveShadow(i));if(invalidate)e.identity=0;}}
    return STATUS_SUCCESS;
}
void GpuPath::ForgetShadow(uint64_t identity){
    for(auto&e:shadows)if(e.identity==identity)e.identity=0;
    for(auto&e:textureCache.entries)if(e.identity==identity)e.identity=0;
}
NTSTATUS GpuPath::AcquireShadow(uint64_t identity,uint64_t offset,const Pi5AllocationInfo&info,uint32_t&index,bool preserve){
    index=UINT32_MAX;
    if(!identity||info.Dimension!=3||info.Levels!=1||!Pi5CpuInvisible(info))return STATUS_SUCCESS;
    // MMIO presentation runs at DIRQL and cannot resolve a tiled shadow.
    // Only displayable allocations stay linear. DWM also uses PRESENT-bound
    // mode-sized off-screen textures; these must retain their tiled cache.
    if(linearWidth&&(info.MiscFlags&PI5_RESOURCE_DISPLAYABLE)&&
       (info.Format==87||info.Format==88))return STATUS_SUCCESS;
    index=FindShadow(identity,offset);if(index!=UINT32_MAX)return STATUS_SUCCESS;
    pi5::TextureChain chain;if(!pi5::LayoutTextureChain(info.Width,info.Height,1,chain)||chain.bytes>target.Bytes)return STATUS_SUCCESS;
    // A new allocation at this segment address supersedes any obsolete identity.
    // Paging has already resolved live evictions before reusing their pages.
    for(auto&e:shadows)if(e.identity&&pi5::TileCache::Overlap(offset,info.Bytes,e.source,e.info.Bytes))e.identity=0;
    uint32_t arena=0;
    for(;;){
        uint32_t free=UINT32_MAX,oldest=UINT32_MAX,count=0;pi5::TextureView live[RTL_NUMBER_OF(shadows)]={};
        for(uint32_t i=0;i<RTL_NUMBER_OF(shadows);++i){auto&e=shadows[i];if(!e.identity){if(free==UINT32_MAX)free=i;continue;}
            if(oldest==UINT32_MAX||e.age<shadows[oldest].age)oldest=i;
            live[count++]={e.identity,e.source,1,0,1,e.arena,e.bytes};
        }
        pi5::TextureView view={identity,offset,1,0,1,0,chain.bytes};uint32_t cursor=shadowCursor;
        if(free!=UINT32_MAX&&pi5::PlaceTextureView(textureCache,target.Bytes,cursor,view,false,live,count)){index=free;arena=view.arena;shadowCursor=cursor;break;}
        if(oldest==UINT32_MAX)return STATUS_INSUFFICIENT_RESOURCES;
        TRY(ResolveShadow(oldest));shadows[oldest].identity=0;
    }
    uint32_t rows=chain.level[0].paddedHeight/8;
    if(preserve)TRY(CopySurface(segment.Address+static_cast<ULONG>(offset),target.Address+arena,info.Width,info.Height,info.Pitch,info.Pitch,0,rows));
    shadows[index]={identity,offset,++shadowAge,info,arena,chain.bytes,rows,false};return STATUS_SUCCESS;
}
