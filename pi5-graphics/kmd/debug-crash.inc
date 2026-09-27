// DEBUG-only diagnostics; all storage and code remain resident. The callback
// uses no allocation, locks, waits, floating point or pageable driver state.
static KBUGCHECK_REASON_CALLBACK_RECORD crashRecord;
static BOOLEAN crashRegistered,crashCaptured;
static ULONG crashCode;
static ULONG_PTR crashArguments[4];
static volatile ULONG *crashPixels[2];
static ULONG crashWidth,crashHeight,crashPitch;
static UCHAR crashComponent[]="Pi5Graphics";
static VOID DrawHexRows(volatile ULONG *pixels,ULONG width,ULONG height,ULONG pitch,
                        ULONG bottom,const ULONG (*rows)[5],ULONG count){
    if(!pixels||width<600||height<240||pitch<width*4||bottom>height||count*21>bottom)return;
    static const UCHAR font[16][5]={{7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},{2,5,7,5,5},{6,5,6,5,6},{7,4,4,4,7},{6,5,5,5,6},{7,4,7,4,7},{7,4,7,4,4}};
    for(UINT row=0;row<count;++row)for(UINT col=0;col<5;++col)for(UINT digit=0;digit<8;++digit){ULONG value=(rows[row][col]>>(28-digit*4))&15;
        for(UINT yy=0;yy<18;++yy)for(UINT xx=0;xx<12;++xx){bool on=xx<9&&yy<15&&(font[value][yy/3]&(4u>>(xx/3)));UINT px=20+(col*9+digit)*12+xx,py=height-bottom+row*21+yy;pixels[SIZE_T(py)*(pitch/4)+px]=on?0x00ffff00:0;}}
    KeMemoryBarrier();
}
static VOID DrawCrashArguments(){
    if(!crashCaptured)return;
    ULONG rows[3][5]={{0xbc,crashCode,0,0,0},{0xa0},{0xa1}};
    for(UINT i=0;i<4;++i){rows[1][i+1]=static_cast<ULONG>(crashArguments[i]);rows[2][i+1]=static_cast<ULONG>(crashArguments[i]>>32);}
    for(UINT i=0;i<2;++i)DrawHexRows(crashPixels[i],crashWidth,crashHeight,crashPitch,96,rows,3);
}
static VOID CrashReason(KBUGCHECK_CALLBACK_REASON reason,PKBUGCHECK_REASON_CALLBACK_RECORD record,PVOID data,ULONG length){
    UNREFERENCED_PARAMETER(record);
    if(reason!=KbCallbackTriageDumpData||!data||length<sizeof(KBUGCHECK_TRIAGE_DUMP_DATA))return;
    auto dump=static_cast<PKBUGCHECK_TRIAGE_DUMP_DATA>(data);
    if(!(dump->Flags&KB_TRIAGE_DUMP_DATA_FLAG_BUGCHECK_ACTIVE))return;
    crashCode=dump->BugCheckCode;crashArguments[0]=dump->BugCheckParameter1;crashArguments[1]=dump->BugCheckParameter2;
    crashArguments[2]=dump->BugCheckParameter3;crashArguments[3]=dump->BugCheckParameter4;crashCaptured=TRUE;
    // Pointers are published only by the system-display bugcheck path. If the
    // callback runs first, its next SystemDisplayWrite will paint these values.
    DrawCrashArguments();
}
static VOID ClearCrashDisplay(){crashPixels[0]=crashPixels[1]=nullptr;KeMemoryBarrier();}
static VOID UnregisterCrash(){if(crashRegistered){(void)KeDeregisterBugCheckReasonCallback(&crashRecord);crashRegistered=FALSE;}}
