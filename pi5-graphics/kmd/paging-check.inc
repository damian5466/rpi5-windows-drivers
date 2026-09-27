// DEBUG startup check of the actual paging routine, before VidMm uses the segment.
// Only the first eight newly allocated pages are touched, then cleared again.
static NTSTATUS CheckPaging(Adapter *a) {
    constexpr ULONG bytes = 8 * PAGE_SIZE;
    if (a->memoryBytes < bytes) return STATUS_INVALID_PARAMETER;
    auto storage = static_cast<PUCHAR>(Allocate(bytes));
    if (!storage) return STATUS_INSUFFICIENT_RESOURCES;
    PMDL mdl = IoAllocateMdl(storage, bytes, FALSE, FALSE, nullptr);
    if (!mdl) { Free(storage); return STATUS_INSUFFICIENT_RESOURCES; }
    MmBuildMdlForNonPagedPool(mdl);
    ULONG checks = 0;
    NTSTATUS result = STATUS_SUCCESS;
    auto Pattern = [](ULONG i) { return static_cast<UCHAR>((i * 37u + (i >> 8) * 19u + 11u) & 255u); };
    auto Reset = [&]() {
        for (ULONG i = 0; i < bytes; ++i) { a->memory[i] = 0xa5; storage[i] = Pattern(i); }
    };
    auto Match = [&](bool good) {
        ++checks;
        if (!good) result = STATUS_DATA_ERROR;
    };
    DXGKARG_BUILDPAGINGBUFFER p = {};
    p.Operation = DXGK_OPERATION_TRANSFER;
    p.Transfer.Source.pMdl = mdl;
    p.Transfer.Destination.SegmentId = 1;
    p.Transfer.Destination.SegmentAddress.QuadPart = PAGE_SIZE;
    p.Transfer.TransferOffset = 2 * PAGE_SIZE;
    p.Transfer.MdlOffset = 1;
    p.Transfer.TransferSize = PAGE_SIZE;
    Reset();
    Match(Paging(a, p) == STATUS_SUCCESS);
    for (ULONG i = 0; i < bytes; ++i)
        Match(a->memory[i] == (i >= 3 * PAGE_SIZE && i < 4 * PAGE_SIZE ? Pattern(i - 2 * PAGE_SIZE) : 0xa5));

    // Page-out applies TransferOffset to the segment, and MdlOffset to the MDL.
    p.Transfer.Source.SegmentId = 1;
    p.Transfer.Source.SegmentAddress.QuadPart = PAGE_SIZE;
    p.Transfer.Destination.SegmentId = 0;
    p.Transfer.Destination.pMdl = mdl;
    p.Transfer.MdlOffset = 4;
    for (ULONG i = 0; i < bytes; ++i) { a->memory[i] = Pattern(i); storage[i] = 0xa5; }
    Match(Paging(a, p) == STATUS_SUCCESS);
    for (ULONG i = 0; i < bytes; ++i)
        Match(storage[i] == (i >= 4 * PAGE_SIZE && i < 5 * PAGE_SIZE ? Pattern(i - PAGE_SIZE) : 0xa5));

    // Segment-to-segment transfers apply the offset to both sides.
    p.Transfer.Destination.SegmentId = 1;
    p.Transfer.Destination.SegmentAddress.QuadPart = 4 * PAGE_SIZE;
    for (ULONG i = 0; i < bytes; ++i) a->memory[i] = Pattern(i);
    Match(Paging(a, p) == STATUS_SUCCESS);
    for (ULONG i = 0; i < bytes; ++i)
        Match(a->memory[i] == Pattern(i >= 6 * PAGE_SIZE && i < 7 * PAGE_SIZE ? i - 3 * PAGE_SIZE : i));

    // Invalid segment ranges must fail before writing, including wraparound.
    p.Transfer.Source.SegmentId = 0;
    p.Transfer.Source.pMdl = mdl;
    p.Transfer.MdlOffset = 0;
    p.Transfer.Destination.SegmentAddress.QuadPart = a->memoryBytes - PAGE_SIZE;
    p.Transfer.TransferOffset = PAGE_SIZE;
    Match(Paging(a, p) == STATUS_INVALID_PARAMETER);
    p.Transfer.Destination.SegmentAddress.QuadPart = -1;
    p.Transfer.TransferOffset = 1;
    Match(Paging(a, p) == STATUS_INVALID_PARAMETER);
    p.Transfer.Destination.SegmentAddress.QuadPart = 0;
    p.Transfer.TransferOffset = 0xffffffffu;
    Match(Paging(a, p) == STATUS_INVALID_PARAMETER);
    p.Transfer.TransferOffset = 0;
    p.Transfer.MdlOffset = 8;
    Match(Paging(a, p) == STATUS_INVALID_PARAMETER);

    // The last complete page is a valid boundary on each kind of backing store.
    p.Transfer.Destination.SegmentAddress.QuadPart = 0;
    p.Transfer.TransferOffset = 7 * PAGE_SIZE;
    p.Transfer.MdlOffset = 7;
    Reset();
    Match(Paging(a, p) == STATUS_SUCCESS);
    for (ULONG i = 0; i < bytes; ++i)
        Match(a->memory[i] == (i >= 7 * PAGE_SIZE ? Pattern(i) : 0xa5));
    // This MDL already had a system mapping; the transfer must leave it intact.
    Match((mdl->MdlFlags & MDL_SOURCE_IS_NONPAGED_POOL) != 0);
    // An abandoned dirty tiled surface must not be resolved after VidMm
    // discards it. Retire both cache types, retaining an unrelated identity.
    a->gpu.shadows[0].identity=71;a->gpu.shadows[0].dirty=true;
    a->gpu.shadows[1].identity=72;a->gpu.shadows[1].dirty=true;
    a->gpu.textureCache.entries[0].identity=71;
    a->gpu.textureCache.entries[1].identity=72;
    p={};p.Operation=DXGK_OPERATION_DISCARD_CONTENT;
    p.DiscardContent.hAllocation=reinterpret_cast<HANDLE>(ULONG_PTR(71));
    Reset();Match(Paging(a,p)==STATUS_SUCCESS);
    Match(!a->gpu.shadows[0].identity&&!a->gpu.textureCache.entries[0].identity);
    Match(a->gpu.shadows[1].identity==72&&a->gpu.textureCache.entries[1].identity==72);
    for(ULONG i=0;i<bytes;++i)Match(a->memory[i]==0xa5);
    RtlZeroMemory(a->gpu.shadows,sizeof(a->gpu.shadows));a->gpu.textureCache={};
    for (ULONG i = 0; i < bytes; ++i) a->memory[i] = 0;
    KeMemoryBarrier();
    IoFreeMdl(mdl);
    Free(storage);
    return Pi5Trace(75, result, checks);
}
