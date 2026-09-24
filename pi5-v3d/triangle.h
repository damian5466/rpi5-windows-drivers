/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
/* Fixed V3D 7.1.10 command/state encodings and branch-free shaders.
 * All addresses refer to this driver's guarded GPU mappings. */
static const uint8_t TriangleGeneric[] = {
    0x7d, 0x1a, 0x38, 0x02, 0x36, 0x00, 0x00, 0x00, 0x00, 0x15, 0x00, 0x1d, 
    0x00, 0xb0, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 
    0x1b, 0x12, 
};
static const uint8_t TriangleRcl[] = {
    0x79, 0x00, 0x40, 0x00, 0x20, 0x00, 0x50, 0x30, 0x01, 0x79, 0x02, 0x00, 
    0x7c, 0x40, 0x00, 0x00, 0x00, 0x00, 0x79, 0x01, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x7e, 0x04, 0x7b, 0x00, 0x00, 0x05, 0x00, 0x7a, 0x00, 
    0x00, 0x01, 0x01, 0x01, 0x10, 0x00, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x1a, 
    0x1d, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x19, 0x1b, 0x7c, 0x00, 0x00, 0x00, 0x1a, 0x1d, 0x08, 0x00, 0x00, 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1b, 0x13, 0x14, 
    0x00, 0x02, 0x03, 0x00, 0x1a, 0x02, 0x03, 0x00, 0x17, 0x00, 0x00, 0x0d, 
};
static const uint8_t TriangleBcl[] = {
    0x77, 0x00, 0x78, 0x10, 0x13, 0x00, 0x00, 0x3f, 0x00, 0x1f, 0x00, 0x13, 
    0x5c, 0x00, 0x00, 0x00, 0x00, 0x06, 0x6b, 0x00, 0x00, 0x00, 0x00, 0x40, 
    0x00, 0x20, 0x00, 0x6c, 0x00, 0x20, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 
    0x6e, 0x00, 0x00, 0x00, 0x45, 0x00, 0x00, 0x80, 0x44, 0x6f, 0x00, 0x00, 
    0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f, 0x6d, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x80, 0x3f, 0x60, 0x03, 0x70, 0x00, 0x57, 0xf0, 0xff, 0xff, 0xff, 
    0x5b, 0x0f, 0x00, 0x80, 0x3f, 0x61, 0x63, 0x58, 0x47, 0x22, 0x40, 0x02, 
    0x03, 0x03, 0x00, 0x24, 0x04, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
    0x00, 0x04, 
};
static const uint8_t TriangleState[] = {
    0x02, 0x20, 0x20, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x08, 0x03, 0x00, 
    0x00, 0x00, 0x04, 0x00, 0x01, 0x06, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 
    0x01, 0x04, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 
    0x98, 0x04, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 
    0x10, 0x00, 0x01, 0x00, 0x98, 0x42, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 
    0x02, 0x00, 0x00, 0x00, 
};

static const uint64_t TriangleCs[] = {
    UINT64_C(0x39c02180bc03f000), /* ldvpmv_in rf0, 0              ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02181bc03f040), /* ldvpmv_in rf1, 1              ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02182bc03f080), /* ldvpmv_in rf2, 2              ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02183bc03f0c0), /* ldvpmv_in rf3, 3              ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02184bc03f100), /* ldvpmv_in rf4, 4              ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02185bc03f140), /* ldvpmv_in rf5, 5              ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02180be03f000), /* stvpmv 0, rf0                 ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02180be03f041), /* stvpmv 1, rf1                 ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02180be03f082), /* stvpmv 2, rf2                 ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02180be03f0c3), /* stvpmv 3, rf3                 ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02180be03f104), /* stvpmv 4, rf4                 ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02180be03f145), /* stvpmv 5, rf5                 ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
};
static const uint64_t TriangleVs[] = {
    UINT64_C(0x39c02180bc03f000), /* ldvpmv_in rf0, 0              ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02181bc03f040), /* ldvpmv_in rf1, 1              ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02182bc03f080), /* ldvpmv_in rf2, 2              ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02183bc03f0c0), /* ldvpmv_in rf3, 3              ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02180be03f000), /* stvpmv 0, rf0                 ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02180be03f041), /* stvpmv 1, rf1                 ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02180be03f082), /* stvpmv 2, rf2                 ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39c02180be03f0c3), /* stvpmv 3, rf3                 ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
};
static const uint64_t TriangleFs[] = {
    UINT64_C(0x39803186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf0 */
    UINT64_C(0x39807186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf1 */
    UINT64_C(0x3980b186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf2 */
    UINT64_C(0x3980f186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf3 */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003188b603f000), /* or tlbu, rf0, rf0             ; nop */
    UINT64_C(0x38003187b603f041), /* or tlb, rf1, rf1              ; nop */
    UINT64_C(0x38003187b603f082), /* or tlb, rf2, rf2              ; nop */
    UINT64_C(0x38003187b603f0c3), /* or tlb, rf3, rf3              ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
};
