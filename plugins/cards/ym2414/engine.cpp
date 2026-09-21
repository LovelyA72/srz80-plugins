#include "engine.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "opz_core.h"

// The vendored core leaves its interface stubs' parameters unnamed; silence
// that one warning class for the includes only (the project builds -Wextra).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "ymfm/ymfm_opz.h"
#pragma GCC diagnostic pop

namespace srz80::ym2414 {
namespace {

struct YmfmInterface final : ymfm::ymfm_interface {};

// ymfm keeps its register array private, so we mirror the same bookkeeping it
// performs on writes (including the "preset" redirect) to keep the raw-register
// view faithful for the ymfm backend.
void apply_register_image(uint8_t (&regs)[Engine::kRegisters], uint32_t index, uint8_t data) {
    if (index == 0x17 && (data & 0x80) != 0) {
        regs[0x188] = data;
    } else if (index == 0x19 && (data & 0x80) != 0) {
        regs[0x189] = data;
    } else if ((index & 0xe0) == 0x40 && (data & 0x80) != 0) {
        regs[0x100 + (index & 0x1f)] = data;
    } else if ((index & 0xe0) == 0xc0 && (data & 0x20) != 0) {
        regs[0x120 + (index & 0x1f)] = data;
    } else if (index < 0x100) {
        regs[index] = data;
    }

    if ((regs[0x100 + (index & 0x1f)] & 0x80) != 0) {
        if ((index & 0xe0) == 0xe0) {
            regs[0x140 + (index & 0x1f)] = data;
            regs[0x100 + (index & 0x1f)] &= 0x7f;
        } else if ((index & 0xe0) == 0xc0 && (data & 0x20) != 0) {
            regs[0x160 + (index & 0x1f)] = data;
        }
    }
}

int16_t clamp16(int32_t value) {
    return static_cast<int16_t>(std::clamp(value, -32768, 32767));
}

} // namespace

struct Engine::Impl {
    Backend backend = Backend::srz80;
    uint32_t chip_clock_hz = 0;
    uint8_t address = 0;
    std::unique_ptr<Core> core;
    YmfmInterface ymfm_interface;
    std::unique_ptr<ymfm::ym2414> ymfm;
    uint8_t regs[kRegisters]{};
    int32_t output[2] = {0, 0};
    uint64_t ymfm_state_size = 0;
};

Engine::Engine(Backend backend, uint32_t chip_clock_hz) : impl_(std::make_unique<Impl>()) {
    impl_->backend = backend;
    impl_->chip_clock_hz = chip_clock_hz;
    if (backend == Backend::ymfm)
        impl_->ymfm = std::make_unique<ymfm::ym2414>(impl_->ymfm_interface);
    else
        impl_->core = std::make_unique<Core>(chip_clock_hz);
    reset();
}

Engine::~Engine() = default;

void Engine::reset() {
    impl_->address = 0;
    std::fill(std::begin(impl_->regs), std::end(impl_->regs), 0);
    impl_->output[0] = impl_->output[1] = 0;
    if (impl_->backend == Backend::ymfm)
        impl_->ymfm->reset();
    else
        impl_->core->reset();
}

void Engine::write_address(uint8_t value) {
    impl_->address = value;
    if (impl_->backend == Backend::ymfm)
        impl_->ymfm->write_address(value);
    else
        impl_->core->write_address(value);
}

void Engine::write_data(uint8_t value) {
    if (impl_->backend == Backend::ymfm) {
        apply_register_image(impl_->regs, impl_->address, value);
        impl_->ymfm->write_data(value);
    } else {
        impl_->core->write_data(value);
    }
}

uint8_t Engine::read_status() {
    return impl_->backend == Backend::ymfm ? impl_->ymfm->read_status()
                                           : impl_->core->read_status();
}

uint32_t Engine::native_rate() const {
    return impl_->backend == Backend::ymfm ? impl_->ymfm->sample_rate(impl_->chip_clock_hz)
                                           : impl_->core->native_rate();
}

void Engine::clock() {
    if (impl_->backend == Backend::ymfm) {
        ymfm::ym2414::output_data generated{};
        impl_->ymfm->generate(&generated, 1);
        impl_->output[0] = generated.data[0];
        impl_->output[1] = generated.data[1];
    } else {
        impl_->core->clock();
    }
}

int16_t Engine::output_left() const {
    return impl_->backend == Backend::ymfm ? clamp16(impl_->output[0]) : impl_->core->output_left();
}

int16_t Engine::output_right() const {
    return impl_->backend == Backend::ymfm ? clamp16(impl_->output[1]) : impl_->core->output_right();
}

uint32_t Engine::operator_env(uint32_t op) {
    if (impl_->backend == Backend::ymfm)
        return impl_->ymfm->debug_engine()->debug_operator(op)->debug_eg_attenuation();
    return impl_->core->operator_env(op);
}

uint32_t Engine::operator_state(uint32_t op) {
    if (impl_->backend == Backend::srz80)
        return impl_->core->operator_state(op);
    // Map ymfm's envelope states onto the card's attack..reverb view.
    switch (impl_->ymfm->debug_engine()->debug_operator(op)->debug_eg_state()) {
    case ymfm::EG_DECAY:
        return 1;
    case ymfm::EG_SUSTAIN:
        return 2;
    case ymfm::EG_RELEASE:
        return 3;
    case ymfm::EG_REVERB:
        return 4;
    default:
        return 0; // EG_ATTACK (EG_DEPRESS does not occur on OPZ)
    }
}

int32_t Engine::operator_output(uint32_t op) {
    if (impl_->backend == Backend::srz80)
        return impl_->core->operator_output(op);
    auto *engine = impl_->ymfm->debug_engine();
    auto *oper = engine->debug_operator(op);
    return oper->compute_volume(oper->phase(), engine->regs().lfo_am_offset(oper->choffs()));
}

uint32_t Engine::operator_phase(uint32_t op) {
    if (impl_->backend == Backend::ymfm)
        return impl_->ymfm->debug_engine()->debug_operator(op)->phase();
    return impl_->core->operator_phase(op);
}

uint32_t Engine::operator_key(uint32_t op) {
    if (impl_->backend == Backend::srz80)
        return impl_->core->operator_key(op);
    // ymfm exposes no key latch; report an operator as keyed while its envelope
    // is still in attack/decay/sustain.
    switch (impl_->ymfm->debug_engine()->debug_operator(op)->debug_eg_state()) {
    case ymfm::EG_ATTACK:
    case ymfm::EG_DECAY:
    case ymfm::EG_SUSTAIN:
        return 1;
    default:
        return 0;
    }
}

uint8_t Engine::register_value(uint32_t index) const {
    if (index >= kRegisters)
        return 0;
    if (impl_->backend == Backend::ymfm)
        return impl_->regs[index];
    return impl_->core->register_value(index);
}

uint64_t Engine::state_size() {
    if (impl_->backend == Backend::srz80)
        return Core::state_size();
    if (impl_->ymfm_state_size == 0) {
        // The ymfm blob is structurally fixed; measure it once with a trial save.
        std::vector<uint8_t> buffer;
        ymfm::ymfm_saved_state state(buffer, true);
        impl_->ymfm->save_restore(state);
        impl_->ymfm_state_size = buffer.size();
    }
    // ymfm keeps its register file private, so the raw-register view is kept in
    // a mirror that travels with the blob (see apply_register_image).
    return kRegisters + impl_->ymfm_state_size;
}

void Engine::save_state(uint8_t *buffer) {
    if (impl_->backend == Backend::srz80) {
        impl_->core->save_state(buffer);
        return;
    }
    std::memcpy(buffer, impl_->regs, kRegisters);
    std::vector<uint8_t> blob;
    ymfm::ymfm_saved_state state(blob, true);
    impl_->ymfm->save_restore(state);
    std::memcpy(buffer + kRegisters, blob.data(), blob.size());
}

bool Engine::load_state(const uint8_t *buffer, uint64_t size) {
    if (impl_->backend == Backend::srz80)
        return impl_->core->load_state(buffer, size);
    if (!buffer || size != state_size())
        return false;
    // Validate the blob before committing either the mirror or the device.
    std::vector<uint8_t> blob(buffer + kRegisters, buffer + size);
    ymfm::ymfm_saved_state state(blob, false);
    impl_->ymfm->save_restore(state);
    std::memcpy(impl_->regs, buffer, kRegisters);
    return true;
}

} // namespace srz80::ym2414
