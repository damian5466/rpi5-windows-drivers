// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "layout.h"

int BcmPinFields(unsigned Revision, unsigned Aon, unsigned Pin, BCM_PIN_FIELDS *Fields)
{
    static const uint8_t mainD0[] = {
        1,2,3,4,10,11,12,13,14,15,18,19,20,21,22,23,
        24,25,26,27,28,29,30,31,32,33,34,35
    };
    static const uint8_t aonD0[] = {0,1,2,3,4,5,6,8,9,12,13,14};
    unsigned i, pull;
    if (!Fields || Revision > 1 || Aon > 1) return 0;
    if (Aon && Pin >= 32 && Pin <= 37) {
        i = Pin - 32;
        Fields->MuxBit = (uint16_t)(i < 4 ? i * 4 : (i - 3) * 32);
        Fields->PullBit = UINT16_MAX;
        return 1;
    }
    if (!Revision) {
        if ((!Aon && Pin >= 54) || (Aon && Pin >= 17)) return 0;
        Fields->MuxBit = (uint16_t)((Aon ? 96 : 0) + Pin * 4);
        pull = Pin + (Aon ? 10 : 7);
        Fields->PullBit = (uint16_t)(((Aon ? 6 : 7) + pull / 15) * 32 + (pull % 15) * 2);
        return 1;
    }
    for (i = 0; i < (Aon ? sizeof(aonD0) : sizeof(mainD0)); ++i) {
        if (Pin == (unsigned)(Aon ? aonD0[i] : mainD0[i])) {
            Fields->MuxBit = (uint16_t)((Aon ? 96 : 0) + i * 4);
            pull = i + (Aon ? 9 : 5);
            Fields->PullBit = (uint16_t)(((Aon ? 5 : 4) + pull / 15) * 32 + (pull % 15) * 2);
            return 1;
        }
    }
    return 0;
}
uint64_t BcmValidPins(unsigned Revision, unsigned Aon)
{
    BCM_PIN_FIELDS fields;
    uint64_t mask = 0;
    unsigned pin;
    for (pin = 0; pin < 64; ++pin)
        if (BcmPinFields(Revision, Aon, pin, &fields) && !(Aon && pin == 3))
            mask |= UINT64_C(1) << pin;
    return mask;
}
