// Compare the TFU with the independently implemented CPU texture addressing.
// Includes small utiles, both UB strides, odd dimensions, padded UIF and XOR,
// bit patterns that represent NaNs, and guards around the complete mip chain.
NTSTATUS GpuPath::CheckTiles(){
    static const ULONG sizes[][2]={{1,1},{3,17},{17,3},{9,9},{17,11},{65,33},{600,544}};
    ULONG checked=0;
    for(const auto&size:sizes){
        ULONG levels=1,largest=size[0]>size[1]?size[0]:size[1];while(largest>>levels)++levels;
        pi5::TextureChain chain;if(!pi5::LayoutTextureChain(size[0],size[1],levels,chain))return STATUS_DATA_ERROR;
        ULONG total=4096+chain.bytes+4096;RtlFillMemory(textureData,total,0xa5);TRY(Transfer(texture,textureData,total,TRUE));
        auto Pattern=[&](ULONG x,ULONG y,ULONG level){return 0xff000000u^((x*0x45d9f3bu+y*0x119de1f3u+level*97u+size[0])&0x00ffffffu);};
        for(ULONG i=0;i<levels;++i){const auto&level=chain.level[i];ULONG pitch=(level.width*4+63)&~63u;
            RtlFillMemory(textureData,pitch*level.height+64,0x3c);auto words=reinterpret_cast<ULONG*>(textureData+64);
            for(ULONG y=0;y<level.height;++y)for(ULONG x=0;x<level.width;++x)words[y*(pitch/4)+x]=Pattern(x,y,i);
            TRY(Transfer(source,textureData,pitch*level.height+64,TRUE));
            PI5_V3D_BUFFER_TILE convert={};convert.Version=1;convert.Source=source.Handle;convert.Destination=texture.Handle;convert.SourceOffset=64;
            convert.DestinationOffset=4096+level.offset;convert.Width=level.width;convert.Height=level.height;convert.SourcePitch=pitch;
            convert.Layout=level.tiling==pi5::Tiling::Linear?3:level.tiling==pi5::Tiling::UbLinear1?4:level.tiling==pi5::Tiling::UbLinear2?5:level.xorEnabled?7:6;
            convert.OutputRows=level.tiling==pi5::Tiling::Uif?level.paddedHeight/8:0;
            TRY(Io(IOCTL_PI5_V3D_BUFFER_TILE,&convert,sizeof(convert),&convert,sizeof(convert)));
        }
        TRY(Transfer(texture,textureData,total,FALSE));
        for(ULONG i=0;i<4096;++i)if(textureData[i]!=0xa5||textureData[4096+chain.bytes+i]!=0xa5)return Pi5Trace(77,STATUS_DATA_ERROR,size[0],i);
        for(ULONG i=0;i<levels;++i){const auto&level=chain.level[i];
            for(ULONG y=0;y<level.height;++y)for(ULONG x=0;x<level.width;++x){ULONG value=*reinterpret_cast<ULONG*>(textureData+4096+level.offset+pi5::TexelOffset(level,x,y));
                if(value!=Pattern(x,y,i))return Pi5Trace(77,STATUS_DATA_ERROR,(size[0]<<16)|i,(y<<16)|x);++checked;}
        }
    }
    PI5_V3D_BUFFER_TILE invalid={};invalid.Version=1;invalid.Source=source.Handle;invalid.Destination=texture.Handle;invalid.Width=invalid.Height=1;invalid.SourcePitch=64;invalid.Layout=7;invalid.OutputRows=1;
    if(Io(IOCTL_PI5_V3D_BUFFER_TILE,&invalid,sizeof(invalid),&invalid,sizeof(invalid))!=STATUS_INVALID_PARAMETER)return STATUS_DATA_ERROR;
    static const ULONG copies[][2]={{16,1},{80,69},{1920,1080},{4096,257}};
    for(const auto&size:copies){ULONG bytes=size[0]*size[1]*4,total=bytes+8192;
        RtlFillMemory(textureData,total,0xa5);TRY(Transfer(target,textureData,total,TRUE));
        auto pixels=reinterpret_cast<ULONG*>(textureData+4096);
        for(ULONG i=0;i<bytes/4;++i)pixels[i]=i*0x45d9f3bu+size[1];
        TRY(Transfer(source,textureData,total,TRUE));
        TRY(CopySurface(source.Address+4096,target.Address+4096,size[0],size[1],size[0]*4,size[0]*4));
        TRY(Transfer(target,textureData,total,FALSE));
        for(ULONG i=0;i<4096;++i)if(textureData[i]!=0xa5||textureData[4096+bytes+i]!=0xa5)return Pi5Trace(79,STATUS_DATA_ERROR,size[0],i);
        for(ULONG i=0;i<bytes/4;++i)if(pixels[i]!=i*0x45d9f3bu+size[1])return Pi5Trace(79,STATUS_DATA_ERROR,size[0],i);
        checked+=bytes/4;
    }
    // Linear subrectangles use independently aligned bases and full-image
    // pitches. Verify final tile clipping, every preserved pixel and guards.
    for(ULONG test=0;test<60;++test){
        static const ULONG widths[]={1,3,17,31,63,64,65,127,192,257},heights[]={1,3,7,64,65,95};
        ULONG width=widths[test%10],height=heights[test/10],sx=(test%5)*16,sy=test%29,dx=(test%6)*16,dy=test%47;
        const ULONG sourcePitch=384*4,destinationPitch=448*4,rows=160,total=destinationPitch*rows+8192;
        RtlFillMemory(textureData,total,0xa5);TRY(Transfer(target,textureData,total,TRUE));
        auto pixels=reinterpret_cast<ULONG*>(textureData+4096);
        for(ULONG i=0;i<sourcePitch*rows/4;++i)pixels[i]=i*0x45d9f3bu+test;
        TRY(Transfer(source,textureData,sourcePitch*rows+8192,TRUE));
        TRY(CopySurface(source.Address+4096+sy*sourcePitch+sx*4,target.Address+4096+dy*destinationPitch+dx*4,width,height,sourcePitch,destinationPitch));
        TRY(Transfer(target,textureData,total,FALSE));
        for(ULONG i=0;i<4096;++i)if(textureData[i]!=0xa5||textureData[total-4096+i]!=0xa5)return Pi5Trace(87,STATUS_DATA_ERROR,test,i);
        for(ULONG y=0;y<rows;++y)for(ULONG x=0;x<destinationPitch/4;++x){
            ULONG expected=x>=dx&&x<dx+width&&y>=dy&&y<dy+height?((y-dy+sy)*(sourcePitch/4)+x-dx+sx)*0x45d9f3bu+test:0xa5a5a5a5u;
            if(pixels[y*(destinationPitch/4)+x]!=expected)return Pi5Trace(87,STATUS_DATA_ERROR,test,(y<<16)|x);++checked;
        }
    }
    Pi5Trace(87,STATUS_SUCCESS,60,448*160*60);
    static const ULONG tiledCopies[][2]={{1,1},{3,17},{65,33},{1920,1080},{4096,257}};
    for(const auto&size:tiledCopies){pi5::TextureChain chain;if(!pi5::LayoutTextureChain(size[0],size[1],1,chain))return STATUS_DATA_ERROR;
        ULONG pitch=(size[0]*4+63)&~63u,linearBytes=pitch*size[1],rows=chain.level[0].paddedHeight/8;
        auto Pattern=[&](ULONG x,ULONG y){return (x+y*size[0])*0x45d9f3bu+size[1];};
        RtlFillMemory(textureData,chain.bytes+8192,0xa5);TRY(Transfer(target,textureData,chain.bytes+8192,TRUE));
        RtlFillMemory(textureData,linearBytes+8192,0xa5);
        for(ULONG y=0;y<size[1];++y)for(ULONG x=0;x<size[0];++x)*reinterpret_cast<ULONG*>(textureData+4096+y*pitch+x*4)=Pattern(x,y);
        TRY(Transfer(source,textureData,linearBytes+8192,TRUE));
        for(unsigned phase=0;phase<2;++phase){
            if(!phase){TRY(CopySurface(source.Address+4096,target.Address+4096,size[0],size[1],pitch,pitch,0,rows));}
            else{pi5::Draw clear={};clear.width=size[0];clear.height=size[1];clear.pitch=pitch;clear.target=target.Address+4096;clear.tiledRows=rows;clear.clearColor=0x29be417c;
                pi5::EncodedCommands encoded;const char*error=nullptr;
                if(!pi5::EncodeClear(clear,cl.Address,commands,PI5_UMD_COMMAND_BYTES,encoded,error))return STATUS_DATA_ERROR;
                ULONG aligned=(encoded.bytes+3)&~3u;RtlZeroMemory(commands+encoded.bytes,aligned-encoded.bytes);TRY(Transfer(cl,commands,aligned,TRUE));
                PI5_V3D_SUBMIT_CL submit={};submit.Version=1;submit.RclStart=encoded.renderStart;submit.RclEnd=encoded.renderEnd;
                TRY(Io(IOCTL_PI5_V3D_SUBMIT_CL,&submit,sizeof(submit),&submit,sizeof(submit)));
            }
            TRY(Transfer(target,textureData,chain.bytes+8192,FALSE));
            for(ULONG i=0;i<4096;++i)if(textureData[i]!=0xa5||textureData[4096+chain.bytes+i]!=0xa5)return Pi5Trace(79,STATUS_DATA_ERROR,phase+10,i);
            for(ULONG y=0;y<size[1];++y)for(ULONG x=0;x<size[0];++x){ULONG expected=phase?0x29be417c:Pattern(x,y);
                if(*reinterpret_cast<ULONG*>(textureData+4096+pi5::TexelOffset(chain.level[0],x,y))!=expected)return Pi5Trace(79,STATUS_DATA_ERROR,phase+12,(y<<16)|x);++checked;}
            RtlFillMemory(textureData,linearBytes+8192,0xa5);TRY(Transfer(texture,textureData,linearBytes+8192,TRUE));
            TRY(CopySurface(target.Address+4096,texture.Address+4096,size[0],size[1],pitch,pitch,rows,0));
            TRY(Transfer(texture,textureData,linearBytes+8192,FALSE));
            for(ULONG i=0;i<4096;++i)if(textureData[i]!=0xa5||textureData[4096+linearBytes+i]!=0xa5)return Pi5Trace(79,STATUS_DATA_ERROR,phase+14,i);
            for(ULONG y=0;y<size[1];++y)for(ULONG x=0;x<size[0];++x){ULONG expected=phase?0x29be417c:Pattern(x,y);
                if(*reinterpret_cast<ULONG*>(textureData+4096+y*pitch+x*4)!=expected)return Pi5Trace(79,STATUS_DATA_ERROR,phase+16,(y<<16)|x);++checked;}
        }
    }
    return Pi5Trace(77,STATUS_SUCCESS,checked,RTL_NUMBER_OF(sizes));
}
