/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>

/* Fixed V3D 7.1 programs: one 16-lane workgroup, no branches or indirect
 * addresses. Uniforms: source VA, destination VA, XOR mask, operand.
 * Each lane loads one uint32, applies XOR then ADD/SUB, stores it, waits
 * for TMU writes and ends. Explicit NOPs preserve register/switch hazards.
 * Encodings round-tripped with Mesa's MIT QPU packer at
 * 51048338d3db061641003a4b18daec821792307d. */
static const uint64_t V3dShaderAdd[] = {
    UINT64_C(0x39813186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf4 */
    UINT64_C(0x39817186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf5 */
    UINT64_C(0x3981b186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf6 */
    UINT64_C(0x3981f186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf7 */
    UINT64_C(0x38002182bb03f002), /* eidx rf2                      ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39e021837c03f082), /* shl rf3, rf2, 2               ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x3800318c3803f103), /* add tmua, rf4, rf3            ; nop */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38823186bb03f000), /* nop                           ; nop                         ; ldtmu.rf8 */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38002188b703f206), /* xor rf8, rf8, rf6             ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x380021883803f207), /* add rf8, rf8, rf7             ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x3800318bb603f208), /* or tmud, rf8, rf8             ; nop */
    UINT64_C(0x3800318c3803f143), /* add tmua, rf5, rf3            ; nop */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f00f), /* tmuwt -                       ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
};
static const uint64_t V3dShaderSub[] = {
    UINT64_C(0x39813186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf4 */
    UINT64_C(0x39817186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf5 */
    UINT64_C(0x3981b186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf6 */
    UINT64_C(0x3981f186bb03f000), /* nop                           ; nop                         ; ldunifrf.rf7 */
    UINT64_C(0x38002182bb03f002), /* eidx rf2                      ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x39e021837c03f082), /* shl rf3, rf2, 2               ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x3800318c3803f103), /* add tmua, rf4, rf3            ; nop */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38823186bb03f000), /* nop                           ; nop                         ; ldtmu.rf8 */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38002188b703f206), /* xor rf8, rf8, rf6             ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x380021883c03f207), /* sub rf8, rf8, rf7             ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x3800318bb603f208), /* or tmud, rf8, rf8             ; nop */
    UINT64_C(0x3800318c3803f143), /* add tmua, rf5, rf3            ; nop */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f00f), /* tmuwt -                       ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38203186bb03f000), /* nop                           ; nop                         ; thrsw */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
    UINT64_C(0x38003186bb03f000), /* nop                           ; nop */
};
