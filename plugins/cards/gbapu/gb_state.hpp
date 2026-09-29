#pragma once
#include <state.hpp>
#include <cmath>
#include "gb.h"

// Serialize the standalone APU's fields, never native padding or callbacks.
template <class Archive> void archive_gb(Archive &ar, GB_gameboy_t &gb) {
    uint32_t model = gb.model;
    ar(model);
    gb.model = static_cast<GB_model_t>(model);
    ar.fields(gb.cgb_mode, gb.cgb_double_speed, gb.halted, gb.stopped,
              gb.io_registers, gb.div_cycles, gb.div_state, gb.div_counter, gb.tima_reload_state);
    auto &a = gb.apu;
    ar.fields(a.global_enable, a.apu_cycles, a.samples, a.is_active, a.div_divider, a.lf_div,
              a.square_sweep_countdown, a.square_sweep_calculate_countdown, a.sweep_length_addend,
              a.shadow_sweep_sample_length, a.unshifted_sweep, a.enable_zombie_calculate_stepping);
    for (auto &s : a.square_channels)
        ar.fields(s.pulse_length, s.current_volume, s.volume_countdown, s.current_sample_index,
                  s.sample_countdown, s.sample_length, s.length_enabled);
    auto &w = a.wave_channel;
    ar.fields(w.enable, w.pulse_length, w.shift, w.sample_length, w.length_enabled,
              w.double_length, w.bank_select, w.force_3, w.sample_countdown,
              w.current_sample_index, w.current_sample, w.wave_form, w.wave_form_just_read);
    auto &n = a.noise_channel;
    ar.fields(n.pulse_length, n.current_volume, n.volume_countdown, n.lfsr, n.narrow,
              n.counter_countdown, n.counter, n.length_enabled, n.alignment);
    ar.fields(a.skip_div_event, a.current_lfsr_sample, a.pcm_mask, a.channel_1_restart_hold,
              a.channel_4_delta, a.channel_4_countdown_reloaded, a.channel_4_dmg_delayed_start,
              a.channel1_completed_addend);
    const auto envelope = [&](GB_envelope_clock_t &e) {
        bool locked = e.locked, clock = e.clock;
        ar.fields(locked, clock);
        e.locked = locked;
        e.clock = clock;
    };
    for (auto &e : a.square_envelope_clock) envelope(e);
    envelope(a.noise_envelope_clock);
    auto &o = gb.apu_output;
    static_assert(sizeof(o.sample_rate) == 4 && sizeof(o.cycles_since_render) == 4);
    uint32_t highpass = o.highpass_mode;
    ar.fields(o.sample_rate, o.sample_cycles, o.cycles_per_sample, o.cycles_since_render,
              o.last_update);
    for (auto &s : o.current_sample) ar.fields(s.left, s.right);
    for (auto &s : o.summed_samples) ar.fields(s.left, s.right);
    ar.fields(o.dac_discharge, highpass, o.highpass_rate, o.highpass_diff.left,
              o.highpass_diff.right, o.final_sample.left, o.final_sample.right,
              o.rate_set_in_clocks, o.interference_volume, o.interference_highpass);
    o.highpass_mode = static_cast<GB_highpass_mode_t>(highpass);
}

inline bool valid_gb_state(const GB_gameboy_t &gb) {
    const auto &a = gb.apu;
    const auto &o = gb.apu_output;
    if (gb.div_state < 0 || gb.div_state > 3 || gb.tima_reload_state > GB_TIMA_RELOADED ||
        a.skip_div_event > GB_SKIP_DIV_EVENT_SKIP || a.wave_channel.shift > 4 ||
        a.wave_channel.current_sample_index >= 64 || a.wave_channel.sample_length > 0x7ff ||
        a.noise_channel.current_volume > 15 ||
        o.highpass_mode >= GB_HIGHPASS_MAX || !std::isfinite(o.sample_cycles) ||
        !std::isfinite(o.cycles_per_sample) || o.cycles_per_sample <= 0 ||
        !std::isfinite(o.highpass_rate) || !std::isfinite(o.highpass_diff.left) ||
        !std::isfinite(o.highpass_diff.right) || !std::isfinite(o.interference_volume) ||
        !std::isfinite(o.interference_highpass)) return false;
    for (const auto &s : a.square_channels)
        if (s.current_volume > 15 || s.sample_length > 0x7ff) return false;
    for (double value : o.dac_discharge) if (!std::isfinite(value)) return false;
    return true;
}
