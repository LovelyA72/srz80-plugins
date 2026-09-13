#include "ymw258.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace srz80::ymw258 {
namespace {
constexpr uint32_t fraction_one = 1u << 16;
constexpr uint32_t gain_one = 1u << 16;
constexpr uint32_t envelope_one = 0x400u << 16;
constexpr double decay_rate_ratio = 14.32833;
constexpr std::array<double, 8> lfo_hz{0.168, 2.019, 3.196, 4.206, 5.215, 5.888, 6.224, 7.066};
constexpr std::array<double, 8> vibrato_cents{0.0, 3.378, 5.065, 6.750, 10.114, 20.170, 40.180, 79.307};
constexpr std::array<double, 8> tremolo_db{0.0, 0.4, 0.8, 1.5, 3.0, 6.0, 12.0, 24.0};
constexpr std::array<double, 64> envelope_times_ms{
    0, 0, 0, 0, 6222.95, 4978.37, 4148.66, 3556.01,
    3111.47, 2489.21, 2074.33, 1778.00, 1555.74, 1244.63, 1037.19, 889.02,
    777.87, 622.31, 518.59, 444.54, 388.93, 311.16, 259.32, 222.27,
    194.47, 155.60, 129.66, 111.16, 97.23, 77.82, 64.85, 55.60,
    48.62, 38.91, 32.43, 27.80, 24.31, 19.46, 16.24, 13.92,
    12.15, 9.75, 8.12, 6.98, 6.08, 4.90, 4.08, 3.49,
    3.04, 2.49, 2.13, 1.90, 1.72, 1.41, 1.18, 1.04,
    0.91, 0.73, 0.59, 0.50, 0.45, 0.45, 0.45, 0.45};

struct DspTables {
    std::array<std::array<uint32_t, 256>, 8> pitch_gain{};
    std::array<std::array<uint32_t, 256>, 8> amplitude_gain{};
    std::array<uint32_t, 1024> envelope_gain{};
    std::array<std::array<uint32_t, 128>, 16> left_gain{};
    std::array<std::array<uint32_t, 128>, 16> right_gain{};

    DspTables() {
        for (size_t depth = 0; depth < 8; ++depth) {
            for (int scale_index = 0; scale_index < 256; ++scale_index) {
                const double cents = vibrato_cents[depth] * (scale_index - 128) / 128.0;
                pitch_gain[depth][scale_index] = static_cast<uint32_t>(
                    std::exp2(cents / 1200.0) * gain_one);
                const double attenuation = tremolo_db[depth] * scale_index / 256.0;
                amplitude_gain[depth][scale_index] = static_cast<uint32_t>(
                    std::pow(10.0, -attenuation / 20.0) * gain_one);
            }
        }
        for (size_t volume = 0; volume < envelope_gain.size(); ++volume) {
            const double db = -96.0 + 96.0 * static_cast<double>(volume) / 1024.0;
            envelope_gain[volume] = static_cast<uint32_t>(std::pow(10.0, db / 20.0) * gain_one);
        }
        for (size_t pan = 0; pan < left_gain.size(); ++pan) {
            double pan_left = 1.0, pan_right = 1.0;
            if (pan >= 1 && pan <= 6) pan_left = std::pow(10.0, -(pan * 3.0) / 20.0);
            else if (pan == 7) pan_left = 0;
            else if (pan == 8) pan_left = pan_right = 0;
            else if (pan == 9) pan_right = 0;
            else if (pan >= 10) pan_right = std::pow(10.0, -((16 - pan) * 3.0) / 20.0);
            for (size_t level = 0; level < left_gain[pan].size(); ++level) {
                const double total = std::pow(10.0, -(level * 0.375) / 20.0);
                left_gain[pan][level] = static_cast<uint32_t>(pan_left * total * gain_one);
                right_gain[pan][level] = static_cast<uint32_t>(pan_right * total * gain_one);
            }
        }
    }
};

const DspTables &dsp_tables() {
    static const DspTables tables;
    return tables;
}

template <typename T, bool = std::is_enum_v<T>> struct StoredType { using type = T; };
template <typename T> struct StoredType<T, true> { using type = std::underlying_type_t<T>; };
template <typename T> using StoredTypeT = typename StoredType<T>::type;

template <typename T> void append(std::vector<uint8_t> &out, T value) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
    using U = StoredTypeT<T>;
    auto bits = static_cast<std::make_unsigned_t<U>>(static_cast<U>(value));
    for (size_t byte = 0; byte < sizeof(T); ++byte)
        out.push_back(static_cast<uint8_t>(bits >> (byte * 8)));
}

template <typename T> bool take(std::span<const uint8_t> &in, T &value) {
    static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
    if (in.size() < sizeof(T)) return false;
    using U = StoredTypeT<T>;
    std::make_unsigned_t<U> bits = 0;
    for (size_t byte = 0; byte < sizeof(T); ++byte)
        bits |= static_cast<std::make_unsigned_t<U>>(in[byte]) << (byte * 8);
    value = static_cast<T>(static_cast<U>(bits));
    in = in.subspan(sizeof(T));
    return true;
}

int16_t clamp_sample(int64_t value) {
    return static_cast<int16_t>(std::clamp<int64_t>(value, INT16_MIN, INT16_MAX));
}
} // namespace

Engine::Engine(std::span<const uint8_t> rom, uint32_t chip_clock_hz)
    : rom_(address_space_size, 0) {
    std::copy_n(rom.begin(), std::min<size_t>(rom.size(), rom_.size()), rom_.begin());
    const double native_rate = static_cast<double>(chip_clock_hz) / 224.0;
    for (size_t rate = 0; rate < lfo_phase_steps_.size(); ++rate)
        lfo_phase_steps_[rate] = static_cast<uint32_t>(
            lfo_hz[rate] * (static_cast<double>(UINT32_MAX) / native_rate));
    (void)dsp_tables();
    reset();
}

void Engine::reset() {
    voices_ = {};
    for (auto &voice : voices_) {
        voice.length = 1;
        voice.step = fraction_one;
    }
}

int Engine::decode_slot(uint8_t encoded) {
    if (encoded >= 0x20 || (encoded & 7u) == 7u) return -1;
    return static_cast<int>((encoded >> 3) * 7 + (encoded & 7u));
}

uint8_t Engine::read_rom(uint32_t address) const { return rom_[address & (address_space_size - 1)]; }

void Engine::load_sample(Voice &voice) {
    const uint32_t index = voice.regs[1] | ((voice.regs[2] & 1u) << 8);
    const uint32_t header = index * 12;
    const uint32_t encoded_start = (static_cast<uint32_t>(read_rom(header)) << 16) |
                                   (static_cast<uint32_t>(read_rom(header + 1)) << 8) |
                                   read_rom(header + 2);
    voice.format = (encoded_start & 0x400000u) != 0 ? 1 : 0;
    voice.start = encoded_start & 0x3fffffu;
    voice.loop = (static_cast<uint32_t>(read_rom(header + 3)) << 8) | read_rom(header + 4);
    const uint32_t end = (static_cast<uint32_t>(read_rom(header + 5)) << 8) | read_rom(header + 6);
    voice.length = 0x10000u - end;
    if (!voice.length) voice.length = 0x10000;
    voice.loop = std::min(voice.loop, voice.length - 1);
    voice.regs[6] = read_rom(header + 7);
    voice.attack = read_rom(header + 8) >> 4;
    voice.decay1 = read_rom(header + 8) & 0xf;
    voice.decay_level = read_rom(header + 9) >> 4;
    voice.decay2 = read_rom(header + 9) & 0xf;
    voice.rate_correction = read_rom(header + 10) >> 4;
    voice.release = read_rom(header + 10) & 0xf;
    voice.regs[7] = read_rom(header + 11) & 0xfu;
    voice.phase = 0;
    voice.previous_sample = 0;
    if (voice.playing) key_on(voice);
}

void Engine::update_pitch(Voice &voice) {
    const uint32_t f_number = ((voice.regs[3] & 0xfu) << 6) | (voice.regs[2] >> 2);
    const int octave = static_cast<int>((voice.regs[3] >> 4) ^ 8u) - 8;
    uint64_t step = static_cast<uint64_t>(f_number + 1024) << 5;
    // The chip's encoded -8 octave wraps through the (octave - 1) hardware
    // shifter and therefore selects +7 rather than acting as a mute/freeze.
    if (octave == -8)
        step <<= 8;
    else if (octave >= 0)
        step <<= octave;
    else
        step >>= -octave;
    voice.step = static_cast<uint32_t>(std::min<uint64_t>(step, UINT32_MAX));
}

void Engine::key_on(Voice &voice) {
    voice.playing = true;
    voice.phase = 0;
    voice.previous_sample = 0;
    voice.total_level = static_cast<uint32_t>(voice.target_level) << 16;
    voice.envelope_volume = 0;
    voice.envelope = EnvelopeStage::attack;
    calculate_envelope(voice);
}

void Engine::write(uint8_t encoded_slot, uint8_t reg, uint8_t value) {
    const int selected = decode_slot(encoded_slot);
    if (selected < 0 || reg >= register_count) return;
    auto &voice = voices_[static_cast<size_t>(selected)];
    voice.regs[reg] = value;
    switch (reg) {
    case 0: break;
    case 1: load_sample(voice); break;
    case 2:
    case 3: update_pitch(voice); break;
    case 4:
        if (value & 0x80u) {
            key_on(voice);
        } else if (voice.playing) {
            if (voice.release == 15) {
                voice.playing = false;
                voice.envelope = EnvelopeStage::off;
            } else {
                voice.envelope = EnvelopeStage::release;
            }
        }
        break;
    case 5:
        voice.target_level = value >> 1;
        if (value & 1u) voice.total_level = static_cast<uint32_t>(voice.target_level) << 16;
        break;
    default: break;
    }
}

void Engine::advance_total_level(Voice &voice) {
    const uint32_t target = static_cast<uint32_t>(voice.target_level) << 16;
    if (voice.total_level == target) return;
    // Reference ramp times are 78.2 ms toward louder levels and twice that
    // toward quieter levels, on the chip's nominal 44.1 kHz timebase.
    constexpr uint32_t louder_step = static_cast<uint32_t>((128u << 16) / (78.2 * 44.1));
    constexpr uint32_t quieter_step = static_cast<uint32_t>((128u << 16) / (156.4 * 44.1));
    if (voice.total_level > target)
        voice.total_level = voice.total_level - target <= louder_step ? target : voice.total_level - louder_step;
    else
        voice.total_level = target - voice.total_level <= quieter_step ? target : voice.total_level + quieter_step;
}

void Engine::calculate_envelope(Voice &voice) {
    const int octave = static_cast<int>((voice.regs[3] >> 4) ^ 8u) - 8;
    const uint32_t pitch = ((voice.regs[3] & 0xfu) << 6) | (voice.regs[2] >> 2);
    const int correction = voice.rate_correction == 15
                               ? 0
                               : (octave + static_cast<int>(voice.rate_correction)) * 2 +
                                     ((pitch & 0x200u) ? 1 : 0);
    const auto rate_step = [correction](uint8_t parameter, bool attack) {
        uint32_t rate = 0;
        if (parameter == 15)
            rate = 63;
        else if (parameter != 0)
            rate = static_cast<uint32_t>(std::clamp<int>(parameter * 4 + correction, 0, 63));
        if (rate < 4) return uint32_t{0};
        if (attack && rate == 63) return envelope_one;
        const double samples = envelope_times_ms[rate] * (attack ? 1.0 : decay_rate_ratio) * 44.1;
        return static_cast<uint32_t>(envelope_one / samples);
    };
    voice.attack_step = rate_step(voice.attack, true);
    voice.decay1_step = rate_step(voice.decay1, false);
    voice.decay2_step = rate_step(voice.decay2, false);
    voice.release_step = rate_step(voice.release, false);
}

void Engine::advance_envelope(Voice &voice) {
    if (!voice.playing || voice.envelope == EnvelopeStage::off) return;
    switch (voice.envelope) {
    case EnvelopeStage::attack:
        if (voice.attack_step >= (0x3ffu << 16) - voice.envelope_volume) {
            voice.envelope_volume = 0x3ffu << 16;
            voice.envelope = voice.decay1_step >= envelope_one ? EnvelopeStage::sustain
                                                               : EnvelopeStage::decay;
        } else {
            voice.envelope_volume += voice.attack_step;
        }
        break;
    case EnvelopeStage::decay:
        voice.envelope_volume = voice.decay1_step >= voice.envelope_volume
                                    ? 0
                                    : voice.envelope_volume - voice.decay1_step;
        if ((voice.envelope_volume >> 22) <= 15u - voice.decay_level)
            voice.envelope = EnvelopeStage::sustain;
        break;
    case EnvelopeStage::sustain:
        voice.envelope_volume = voice.decay2_step >= voice.envelope_volume
                                    ? 0
                                    : voice.envelope_volume - voice.decay2_step;
        break;
    case EnvelopeStage::release:
        if (voice.release_step >= voice.envelope_volume) {
            voice.envelope_volume = 0;
            voice.envelope = EnvelopeStage::off;
            voice.playing = false;
        } else {
            voice.envelope_volume -= voice.release_step;
        }
        break;
    case EnvelopeStage::off:
        break;
    }
}

uint32_t Engine::lfo_phase_step(uint8_t rate) const {
    return lfo_phase_steps_[rate & 7u];
}

uint32_t Engine::effective_step(Voice &voice) {
    const uint8_t depth = voice.regs[6] & 7u;
    if (!depth) return voice.step;
    voice.pitch_lfo_phase += lfo_phase_step(voice.regs[6] >> 3);
    const uint32_t index = voice.pitch_lfo_phase >> 24;
    const int32_t scale_index = index < 64    ? static_cast<int32_t>(index * 2 + 128)
                                : index < 128 ? 383 - static_cast<int32_t>(index * 2)
                                : index < 192 ? 384 - static_cast<int32_t>(index * 2)
                                              : static_cast<int32_t>(index * 2) - 383;
    const uint64_t scaled = static_cast<uint64_t>(voice.step) *
                            dsp_tables().pitch_gain[depth][static_cast<size_t>(scale_index)];
    return static_cast<uint32_t>(std::min<uint64_t>(scaled >> 16, UINT32_MAX));
}

int16_t Engine::sample_at(const Voice &voice, uint32_t position) const {
    if (voice.format == 0)
        return static_cast<int16_t>(static_cast<int8_t>(read_rom(voice.start + position)) * 256);
    const uint32_t address = voice.start + (position / 2) * 3;
    uint16_t bits = position & 1u ? static_cast<uint16_t>((read_rom(address + 2) << 4) | (read_rom(address + 1) >> 4))
                                  : static_cast<uint16_t>((read_rom(address) << 4) | (read_rom(address + 1) & 0xf));
    if (bits & 0x800u) bits |= 0xf000u;
    return static_cast<int16_t>(static_cast<int16_t>(bits) * 16);
}

std::array<int16_t, 2> Engine::generate() {
    int64_t left = 0, right = 0;
    const auto &tables = dsp_tables();
    for (auto &voice : voices_) {
        constexpr uint32_t peak_decay = 16; // approximately 46 ms from full scale at 44.1 kHz
        voice.output_peak = voice.output_peak > peak_decay ? voice.output_peak - peak_decay : 0;
        if (!voice.playing) continue;
        uint32_t position = voice.phase >> 16;
        if (position >= voice.length) {
            const uint32_t span = voice.length - voice.loop;
            position = voice.loop + (span ? (position - voice.length) % span : 0);
            voice.phase = (static_cast<uint64_t>(position) << 16) | (voice.phase & 0xffffu);
        }
        const int32_t current_sample = sample_at(voice, position);
        const uint32_t fraction = voice.phase & 0xffffu;
        int64_t sample = (static_cast<int64_t>(current_sample) * fraction +
                          static_cast<int64_t>(voice.previous_sample) * (fraction_one - fraction)) >> 16;
        const uint8_t level = static_cast<uint8_t>(std::min<uint32_t>(voice.total_level >> 16, 127));

        voice.phase += effective_step(voice);
        if (position != voice.phase >> 16) voice.previous_sample = current_sample;
        const uint64_t end = static_cast<uint64_t>(voice.length) << 16;
        if (voice.phase >= end) {
            const uint64_t span = static_cast<uint64_t>(voice.length - voice.loop) << 16;
            voice.phase = (static_cast<uint64_t>(voice.loop) << 16) +
                          (span ? (voice.phase - end) % span : 0);
        }
        advance_total_level(voice);

        const uint8_t am_depth = voice.regs[7] & 7u;
        if (am_depth) {
            voice.amplitude_lfo_phase += lfo_phase_step(voice.regs[6] >> 3);
            const uint32_t index = voice.amplitude_lfo_phase >> 24;
            const uint32_t amplitude = index < 128 ? 255 - index * 2 : index * 2 - 256;
            sample = (sample * tables.amplitude_gain[am_depth][amplitude]) >> 16;
        }

        advance_envelope(voice);
        const uint32_t envelope = std::min<uint32_t>(voice.envelope_volume >> 16, 0x3ff);
        sample = (sample * tables.envelope_gain[envelope]) >> 16;
        const uint8_t pan = voice.regs[0] >> 4;
        const int64_t voice_left = (sample * tables.left_gain[pan][level]) >> 16;
        const int64_t voice_right = (sample * tables.right_gain[pan][level]) >> 16;
        const auto magnitude = static_cast<uint32_t>(std::min<int64_t>(
            std::max(std::abs(voice_left), std::abs(voice_right)), INT16_MAX));
        voice.output_peak = std::max(voice.output_peak, magnitude);
        left += voice_left;
        right += voice_right;
    }
    return {clamp_sample(left), clamp_sample(right)};
}

uint8_t Engine::reg(uint32_t voice, uint32_t index) const {
    return voice < voice_count && index < register_count ? voices_[voice].regs[index] : 0;
}

VoiceInspection Engine::inspect_voice(uint32_t voice) const {
    if (voice >= voice_count) return {};
    const auto &v = voices_[voice];
    return {v.start,
            v.loop,
            v.length,
            std::min<uint32_t>(v.envelope_volume >> 16, 0x3ff),
            std::min<uint32_t>(v.total_level >> 16, 127),
            v.output_peak,
            v.attack,
            v.decay1,
            v.decay2,
            v.decay_level,
            v.rate_correction,
            v.release,
            static_cast<uint8_t>(v.envelope),
            v.playing};
}

std::vector<uint8_t> Engine::save_state() const {
    std::vector<uint8_t> out;
    out.reserve(8 + voices_.size() * 80);
    append(out, uint32_t{0x31574d59});
    append(out, uint32_t{3});
    for (const auto &v : voices_) {
        out.insert(out.end(), v.regs.begin(), v.regs.end());
        for (auto value : {v.start, v.loop, v.length}) append(out, value);
        append(out, v.phase);
        for (auto value : {v.step, v.pitch_lfo_phase, v.amplitude_lfo_phase}) append(out, value);
        append(out, v.previous_sample);
        for (auto value : {v.envelope_volume, v.attack_step, v.decay1_step, v.decay2_step,
                           v.release_step, v.total_level}) append(out, value);
        for (auto value : {v.format, v.attack, v.decay1, v.decay2, v.decay_level, v.rate_correction,
                           v.release, v.target_level}) append(out, value);
        append(out, v.envelope);
        append(out, static_cast<uint8_t>(v.playing));
    }
    return out;
}

bool Engine::load_state(std::span<const uint8_t> state) {
    uint32_t magic = 0, version = 0;
    if (!take(state, magic) || !take(state, version) || magic != 0x31574d59 || version != 3) return false;
    auto restored = voices_;
    for (auto &v : restored) {
        if (state.size() < v.regs.size()) return false;
        std::copy_n(state.begin(), v.regs.size(), v.regs.begin());
        state = state.subspan(v.regs.size());
        if (!take(state, v.start) || !take(state, v.loop) || !take(state, v.length) || !take(state, v.phase) ||
            !take(state, v.step) || !take(state, v.pitch_lfo_phase) || !take(state, v.amplitude_lfo_phase) ||
            !take(state, v.previous_sample) || !take(state, v.envelope_volume) || !take(state, v.attack_step) ||
            !take(state, v.decay1_step) || !take(state, v.decay2_step) || !take(state, v.release_step) ||
            !take(state, v.total_level) || !take(state, v.format) || !take(state, v.attack) ||
            !take(state, v.decay1) || !take(state, v.decay2) || !take(state, v.decay_level) ||
            !take(state, v.rate_correction) || !take(state, v.release) || !take(state, v.target_level) ||
            !take(state, v.envelope)) return false;
        uint8_t playing = 0;
        if (!take(state, playing) || v.start >= address_space_size || !v.length || v.loop >= v.length ||
            v.phase >= (static_cast<uint64_t>(v.length) << 16) || v.format > 1 || v.target_level > 127 ||
            v.total_level > (127u << 16) || v.envelope_volume > (0x3ffu << 16) ||
            static_cast<uint8_t>(v.envelope) > static_cast<uint8_t>(EnvelopeStage::release)) return false;
        v.playing = playing != 0;
        v.output_peak = 0;
    }
    if (!state.empty()) return false;
    voices_ = restored;
    return true;
}

LinearResampler::LinearResampler(Engine &engine, uint32_t chip_clock_hz, uint32_t output_rate)
    : engine_(engine) {
    step_ = (static_cast<uint64_t>(chip_clock_hz) << 32) /
            (static_cast<uint64_t>(224) * output_rate);
    reset();
}

void LinearResampler::reset() {
    phase_ = 0;
    current_ = engine_.generate();
    next_ = engine_.generate();
}

void LinearResampler::render(uint32_t frames, int16_t *interleaved) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (size_t channel = 0; channel < 2; ++channel) {
            const int64_t difference = static_cast<int64_t>(next_[channel]) - current_[channel];
            interleaved[static_cast<size_t>(frame) * 2 + channel] = clamp_sample(
                current_[channel] + ((difference * static_cast<int64_t>(phase_)) >> 32));
        }
        phase_ += step_;
        while (phase_ >= (uint64_t{1} << 32)) {
            phase_ -= uint64_t{1} << 32;
            current_ = next_;
            next_ = engine_.generate();
        }
    }
}

std::vector<uint8_t> LinearResampler::save_state() const {
    std::vector<uint8_t> out;
    append(out, phase_);
    for (auto value : current_) append(out, value);
    for (auto value : next_) append(out, value);
    return out;
}

bool LinearResampler::load_state(std::span<const uint8_t> state) {
    uint64_t phase = 0;
    std::array<int16_t, 2> current{}, next{};
    if (!take(state, phase) || !take(state, current[0]) || !take(state, current[1]) ||
        !take(state, next[0]) || !take(state, next[1]) || !state.empty() || phase >= (uint64_t{1} << 32)) return false;
    phase_ = phase;
    current_ = current;
    next_ = next;
    return true;
}

} // namespace srz80::ymw258
