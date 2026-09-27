#pragma once
#include <stdint.h>
#define PI5_DISPLAY_QUERY_MAGIC 0x50354449u
#define PI5_DISPLAY_QUERY_VERSION 3u
struct PI5_DISPLAY_STATUS {
    uint32_t Magic,Version,Debug,Owned,Fault,ClockMask,Irqs,Notified;
    uint32_t Head,Active,Frame,Scanline,PvControl,PvVideo,PvIntEnable,PvIntStatus;
    uint32_t Width,Height,Pitch,OldHead,OwnHead,ControlCalls,LastEnable,Reserved;
    uint32_t List[10];
    uint64_t ScanoutAddress;
    // Reserved2 is the raw ISR entry count in performance DEBUG builds .45+.
    uint32_t AllocationBytes,GuardFailures,Presents,SourceWidth,SourceHeight,Reserved2;
    uint32_t Samples[4];
};
#define PI5_PRIMARY_QUERY_MAGIC 0x50355052u
#define PI5_RECOVERY_QUERY_MAGIC 0x50355243u
struct PI5_RECOVERY_STATUS {
    uint32_t Magic,Version,ArmFailure,Armed,Fault,Recovering,Recoveries,Submitted,Completed,Reserved;
};
static_assert(sizeof(PI5_RECOVERY_STATUS)==40,"recovery diagnostic layout");
struct PI5_PRIMARY_STATUS {
    uint32_t Magic,Version,Flags,Pending,SourceCalls,Copies,Submitted,Completed;
    uint64_t SourceOffset,SourceIdentity,DisplayedOffset,DisplayedIdentity;
    uint32_t SourcePixel,FramebufferPixel,Width,Height,Reserved[8];
};
static_assert(sizeof(PI5_PRIMARY_STATUS)==112,"primary diagnostic layout");
#define PI5_PERF_QUERY_MAGIC 0x50355046u
enum Pi5PerfMetric {Pi5PerfExecute,Pi5PerfTile,Pi5PerfTextureUpload,Pi5PerfOtherTransfer,
    Pi5PerfGpuWait,Pi5PerfRegion,Pi5PerfScanout,Pi5PerfPaging,Pi5PerfTrace,Pi5PerfCount};
struct PI5_PERF_COUNTER {uint64_t Calls,Ticks,Bytes,MaxTicks;};
struct PI5_PERF_STATUS {
    uint32_t Magic,Version;
    uint64_t Frequency;
    PI5_PERF_COUNTER Counters[Pi5PerfCount];
};
static_assert(sizeof(PI5_PERF_STATUS)==304,"performance diagnostic layout");

#define PI5_JOB_QUERY_MAGIC 0x50354a42u
struct PI5_JOB_SAMPLE {
    uint64_t Sequence,Ticks,EndTicks,RenderTicks,TileTicks,CopyTicks,ScissorPixels,WordPixels;
    uint32_t Operation,Width,Height,ScissorWidth,ScissorHeight,Vertices,PixelWords,PixelHash,BindingBytes,Flags,SkippedLoads;
};
struct PI5_JOB_STATUS {
    uint32_t Magic,Version;
    uint64_t Frequency,Sequence;
    PI5_JOB_SAMPLE Samples[512];
};

#define PI5_GPU_PROFILE_MAGIC 0x50354750u
struct PI5_GPU_PROFILE_SAMPLE {
    uint64_t Sequence,EndTicks;
    uint32_t Lists,Overflow;
    uint64_t Counters[32];
};
struct PI5_GPU_PROFILE_STATUS {
    uint32_t Magic,Version,Enabled,Error;
    uint64_t Sequence;
    PI5_GPU_PROFILE_SAMPLE Samples[128];
};

#define PI5_FRAME_QUERY_MAGIC 0x50354652u
struct PI5_FRAME_SAMPLE {uint64_t Sequence,Ticks;};
struct PI5_FRAME_STATUS {
    uint32_t Magic,Version;
    uint64_t Frequency,Sequence;
    uint32_t Blackouts,Reserved;
    PI5_FRAME_SAMPLE Samples[512];
};

#define PI5_HDMI_QUERY_MAGIC 0x50354844u
// Read-only connector, pixel-valve and scanout-engine diagnostics.
struct PI5_HDMI_STATUS {
    uint32_t Magic,Version,Debug,Reserved;
    uint32_t Hotplug[2],PixelValve[2][12],Hvs[3][8],Hdmi[2][10];
};
static_assert(sizeof(PI5_HDMI_STATUS)==296,"HDMI diagnostic layout");

#define PI5_NATIVE_QUERY_MAGIC 0x50354e44u
// Debug-only, read-only snapshots. Fixed capacity keeps the tool independent
// of the register enum while permitting before/after PHY comparisons.
struct PI5_NATIVE_STATUS {
    uint32_t Magic,Version,Count,Reserved;
    uint32_t Active[2],Current[2][96],Original[2][96];
    uint32_t HvsGlobal[8],Output[2][8],Format[2][10];
    PI5_DISPLAY_STATUS Scanout[2];
    uint32_t GridValid[2],Grid[2][96*54];
};
