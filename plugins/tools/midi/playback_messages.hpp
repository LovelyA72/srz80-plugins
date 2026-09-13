#pragma once
#include "protocol.hpp"

namespace srz80::midi {
// Stop is a sequencer command, not a synthesizer panic. Send channel cleanup
// as well so receivers which ignore real-time transport release held notes.
inline Bytes playback_stop_messages() {
    Bytes bytes{0xfc};
    for (uint8_t channel = 0; channel < 16; ++channel)
        for (uint8_t controller : {uint8_t{64}, uint8_t{123}, uint8_t{120}})
            bytes.insert(bytes.end(), {uint8_t(0xb0 | channel), controller, 0});
    return bytes;
}
}
