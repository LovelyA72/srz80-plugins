#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace srz80::adpcm {
// Card-private playback implementation; no engine, UI, or codec dependencies.
struct Device {
    static constexpr uint32_t ram_size = 2 * 1024 * 1024;
    static constexpr uint32_t register_count = 32;
    enum Mode : uint8_t { dpcm, pcm4, pcm8, ima, mulaw };
    std::vector<uint8_t> ram = std::vector<uint8_t>(ram_size, 0);
    std::array<uint8_t, register_count> regs{};
    uint32_t output_rate = 44100;
    uint32_t start = 0, length = 0, rate = 0, position = 0, mode = 0, phase = 0;
    int32_t predictor = 0, index = 0, sample = 0;
    bool playing = false, error = false;

    Device() { reset(); }
    uint32_t word(uint32_t offset) const {
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(regs[offset + i]) << (8 * i);
        return value;
    }
    void word(uint32_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) regs[offset + i] = uint8_t(value >> (8 * i));
    }
    void reset() {
        regs.fill(0); regs[2] = 255; word(12, 22050);
        start = length = rate = position = mode = phase = 0;
        predictor = index = sample = 0; playing = error = false;
        // Sound RAM survives reset, including the host-uploaded initial bank.
    }
    static uint64_t bytes_needed(uint32_t format, uint32_t samples) {
        if (!samples) return 0;
        if (format == dpcm) return (uint64_t(samples) + 7) / 8;
        if (format == pcm4) return (uint64_t(samples) + 1) / 2;
        if (format == ima) return 4 + uint64_t(samples) / 2;
        return samples;
    }
    void trigger() {
        playing = false; error = false; sample = predictor = index = 0; phase = position = 0;
        mode = regs[0]; start = word(4); length = word(8); rate = word(12);
        if (mode > mulaw || !rate || rate > 192000 || start >= ram_size ||
            bytes_needed(mode, length) > ram_size - start) { error = true; return; }
        if (!length) return;
        if (mode == ima) {
            const uint32_t value = uint32_t(ram[start]) | uint32_t(ram[start + 1]) << 8;
            predictor = value >= 32768 ? int32_t(value) - 65536 : int32_t(value);
            index = ram[start + 2];
            if (index > 88 || ram[start + 3] != 0) { index = 0; error = true; return; }
        }
        playing = true;
        decode(); // The first sample is audible immediately, for one sample period.
    }
    void advance_upload() { word(16, word(16) + 1); }
    uint8_t read(uint32_t offset, bool peek = false) {
        if (offset == 1) return uint8_t((playing ? 1 : 0) | (error ? 128 : 0));
        if (offset == 20) {
            const auto address = word(16);
            if (address >= ram_size) { if (!peek) error = true; return 0; }
            const auto value = ram[address];
            if (!peek) advance_upload();
            return value;
        }
        if (offset >= 24) {
            const uint32_t value = offset < 28 ? position : length - std::min(position, length);
            return uint8_t(value >> (8 * (offset % 4)));
        }
        return regs[offset];
    }
    void write(uint32_t offset, uint8_t value) {
        if (offset == 1) {
            if (value & 2) { playing = false; sample = 0; }
            else if (value & 1) trigger();
            if (value & 128) error = false;
        } else if (offset == 20) {
            const auto address = word(16);
            if (address >= ram_size) { error = true; return; }
            ram[address] = value; advance_upload();
        } else if (offset < 20 && offset != 3) regs[offset] = value;
    }
    void decode() {
        static constexpr int steps[89] = {
            7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,
            88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,
            544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,
            2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,
            9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
        static constexpr int adjustments[8] = {-1,-1,-1,-1,2,4,6,8};
        if (mode == dpcm) {
            const bool bit = (ram[start + position / 8] >> (position % 8)) & 1;
            predictor = std::clamp(predictor + (bit ? 1 : -1), -64, 63);
            sample = predictor * 512;
        } else if (mode == pcm4) {
            const int nibble = (ram[start + position / 2] >> (4 * (position % 2))) & 15;
            sample = (nibble >= 8 ? nibble - 16 : nibble) * 4096;
        } else if (mode == pcm8) {
            const int byte = ram[start + position];
            sample = (byte >= 128 ? byte - 256 : byte) * 256;
        } else if (mode == mulaw) {
            const int code = ram[start + position] ^ 255;
            const int magnitude = ((code & 15) * 8 + 132) << ((code >> 4) & 7);
            sample = (code & 128) ? 132 - magnitude : magnitude - 132;
        } else {
            if (position) {
                const uint32_t n = position - 1;
                const int code = (ram[start + 4 + n / 2] >> (4 * (n % 2))) & 15;
                const int step = steps[index];
                int delta = step >> 3;
                if (code & 1) delta += step >> 2;
                if (code & 2) delta += step >> 1;
                if (code & 4) delta += step;
                predictor = std::clamp(predictor + ((code & 8) ? -delta : delta), -32768, 32767);
                index = std::clamp(index + adjustments[code & 7], 0, 88);
            }
            sample = predictor;
        }
        ++position;
    }
    void render(uint32_t frames, int16_t *out) {
        for (uint32_t frame = 0; frame < frames; ++frame) {
            const auto value = int16_t(playing ? sample * regs[2] / 255 : 0);
            out[2 * size_t(frame)] = out[2 * size_t(frame) + 1] = value;
            if (!playing) continue;
            phase += rate;
            while (phase >= output_rate && playing) {
                phase -= output_rate;
                if (position == length) { playing = false; sample = 0; phase = 0; }
                else decode();
            }
        }
    }
};
} // namespace srz80::adpcm
