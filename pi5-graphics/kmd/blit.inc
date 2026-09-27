#include "blit-program.h"
// Exact IEEE-754 encoding of small integer coordinates without kernel FP use.
static ULONG BlitFloat(LONG value){
    ULONG sign=value<0?0x80000000u:0,n=value<0?static_cast<ULONG>(-value):static_cast<ULONG>(value);
    if(!n)return 0;ULONG top=0;for(ULONG x=n;x>1;x>>=1)++top;
    return sign|((top+127)<<23)|((n<<(23-top))&0x7fffffu);
}
static bool BlitWindow(const Pi5AllocationInfo&source,const Pi5RegionCommand&region,Pi5AllocationInfo&view,ULONG&offset,ULONG&prefix){
    ULONG row=region.SourceY*source.Pitch,origin=row+region.SourceX*4;
    offset=origin&~63u;prefix=(origin-offset)/4;
    ULONG bytes=source.Pitch*region.Height;
    // At the allocation's bottom edge, including a full pitch after a
    // horizontally shifted final row would cross its end. Keep that row's
    // left prefix instead; TFU still converts only the requested height.
    if(offset>source.Bytes||bytes>source.Bytes-offset){offset=row;prefix=region.SourceX;}
    ULONG width=prefix+region.Width;
    if(width>4096||width>source.Pitch/4||offset>source.Bytes||bytes>source.Bytes-offset)return false;
    view=source;view.Width=width;view.Height=region.Height;view.Bytes=bytes;
    view.PrivateFlags=0;view.MiscFlags=0;return true;
}
NTSTATUS GpuPath::BlitRegion(const Pi5RegionCommand&region,const Pi5AllocationInfo*r,void*const*cpu,const uint64_t*identities){
    Pi5PerfScope timing(Pi5PerfRegion,ULONGLONG(region.Width)*region.Height*4);
    ULONG destinationOffset=static_cast<ULONG>(static_cast<UCHAR*>(cpu[0])-segmentCpu);
    bool preserve=region.DestinationX||region.DestinationY||region.Width!=r[0].Width||region.Height!=r[0].Height;
    TRY(AcquireShadow(identities[0],destinationOffset,r[0],activeShadow,preserve));
    if(activeShadow==UINT32_MAX)TRY(SyncShadows(destinationOffset,r[0].Bytes,true));
    ULONG sourceOffset=static_cast<ULONG>(static_cast<UCHAR*>(cpu[1])-segmentCpu);
    uint32_t sourceShadow=FindShadow(identities[1],sourceOffset);
    if(activeShadow==UINT32_MAX&&sourceShadow==UINT32_MAX&&!((region.SourceX|region.DestinationX)&15)){
        // Linear, 64-byte-aligned rectangles can go through the tile buffer
        // directly. Its frame dimensions clip the final tile and preserve all
        // destination pixels outside the rectangle, without a texture upload.
        ULONG input=segment.Address+sourceOffset+region.SourceY*r[1].Pitch+region.SourceX*4;
        ULONG output=segment.Address+destinationOffset+region.DestinationY*r[0].Pitch+region.DestinationX*4;
        KeMemoryBarrier();TRY(CopySurface(input,output,region.Width,region.Height,r[1].Pitch,r[0].Pitch));KeMemoryBarrier();return STATUS_SUCCESS;
    }
    static_assert(sizeof(BlitTemplate)<=4096,"blit packet storage");
    RtlCopyMemory(blitCommand,BlitTemplate,sizeof(BlitTemplate));auto c=reinterpret_cast<Pi5DrawCommand*>(blitCommand);
    Pi5AllocationInfo input=r[1];void*inputCpu=cpu[1];uint64_t inputId=identities[1];
    LONG sx=region.SourceX,sy=region.SourceY,w=region.Width,h=region.Height;
    // Convert only the copied view of a linear source, including direct
    // primaries. Retiling a full desktop for a one-row readback is wasteful.
    // TFU input starts on 64 bytes; include the prefix texels and adjust the
    // fetch coordinates. Keep its conservative full-pitch span in allocation.
    if(sourceShadow==UINT32_MAX){ULONG offset=0,prefix=0;
        if(BlitWindow(r[1],region,input,offset,prefix)){
            inputCpu=static_cast<UCHAR*>(cpu[1])+offset;inputId=0;sx=prefix;sy=0;
        }
    }
    ULONG viewport[]={BlitFloat(region.DestinationX),BlitFloat(region.DestinationY),BlitFloat(region.Width),BlitFloat(region.Height),0,0x3f800000};
    RtlCopyMemory(c->Viewport,viewport,sizeof(viewport));
    ULONG scale[]={BlitFloat(region.Width*32),BlitFloat(-static_cast<LONG>(region.Height*32)),0x3f800000,0};
    for(const auto&p:BlitPatches){ULONG value;
        if(p.kind==2){if(p.value>=4)return STATUS_INVALID_PARAMETER;value=scale[p.value];}
        else if(p.kind==10)value=input.Width;
        else if(p.kind==11)value=input.Height;
        else return STATUS_INVALID_PARAMETER;
        *reinterpret_cast<ULONG*>(blitCommand+p.offset)=value;
    }
    ULONG vertices[]={0xbf800000,0xbf800000,BlitFloat(sx),BlitFloat(sy+h),
        0xbf800000,0x40400000,BlitFloat(sx),BlitFloat(sy-h),
        0x40400000,0xbf800000,BlitFloat(sx+2*w),BlitFloat(sy+h)};
    TRY(Transfer(source,vertices,sizeof(vertices),TRUE));
    Pi5AllocationInfo local[3]={r[0],{PI5_UMD_ABI,4096,sizeof(vertices),1,sizeof(vertices),0,1,1},input};local[1].Levels=1;
    void*localCpu[]={cpu[0],segmentCpu,inputCpu};uint64_t localIds[]={identities[0],0,inputId};
    ResetBatch();TRY(PrepareDraw(*c,local,localCpu,localIds));
    // Vertices live in the provider-owned scratch buffer, not the VidMm segment.
    batchDraws[0].vertexAddress=source.Address;
    TRY(SubmitBatch());KeMemoryBarrier();return STATUS_SUCCESS;
}
