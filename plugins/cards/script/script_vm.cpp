#include "script_card.h"

namespace srz80_script {

ScriptVm::~ScriptVm() {
    deactivate();
}

void ScriptVm::deactivate() {
    for (auto &timer : timers) {
        if (timer.handle && card.host && card.host->cancel)
            card.host->cancel(card.host->context, timer.handle);
        timer.handle = 0;
    }
    for (auto &signal : subscriptions) {
        if (signal.subscription && card.host && card.host->cancel)
            card.host->cancel(card.host->context, signal.subscription);
        signal.subscription = 0;
    }
    active = false;
}

SrhStatus ScriptVm::timer_fired(TimerContext *context) {
    if (!context || context->vm != this)
        return SRH_INVALID;
    const uint64_t id = context->callback_id;
    const SrhStatus result = srz80::sdk::guard([&]() -> SrhStatus {
        uint64_t now = 0;
        if (card.host->time_ns)
            card.host->time_ns(card.host->context, &now);
        std::string error;
        if (!invoke_callback(id, "timer", {}, now, error))
            log_error(current_source, error);
        return SRH_OK;
    });
    release_function(id);
    const auto found = std::find_if(timers.begin(), timers.end(),
                                    [id](const TimerEntry &entry) {
                                        return entry.callback_id == id;
                                    });
    if (found != timers.end())
        timers.erase(found);
    return result;
}

uint64_t ScriptVm::register_timer(uint64_t delay, int function_index,
                                  JSValueConst js_function) {
    const uint64_t id = retain_callback(function_index, js_function);
    if (!id)
        return 0;
    TimerEntry entry;
    entry.callback_id = id;
    entry.delay_ns = delay;
    entry.context = std::make_unique<TimerContext>(TimerContext{this, id});
    timers.push_back(std::move(entry));
    if (active) {
        auto &timer = timers.back();
        const SrhStatus status = card.host->schedule(
            card.host->context, card.owner, delay, [](void *opaque) -> SrhStatus {
                return srz80::sdk::guard([&]() -> SrhStatus {
                auto *context = static_cast<TimerContext *>(opaque);
                return context && context->vm ? context->vm->timer_fired(context) : SRH_INVALID;
                });
            }, timer.context.get(), &timer.handle);
        if (status != SRH_OK) {
            release_function(id);
            timers.pop_back();
            return 0;
        }
    }
    return id;
}

bool ScriptVm::register_signal(std::string_view name, int function_index, std::string &error,
                               JSValueConst js_function) {
    SrhHandle signal = 0;
    if (!card.lookup_signal(name, signal)) {
        error = "Signal not found: " + std::string(name);
        return false;
    }
    const uint64_t id = retain_callback(function_index, js_function);
    if (!id) {
        error = "Expected a signal callback function";
        return false;
    }
    SignalEntry entry;
    entry.callback_id = id;
    entry.signal = signal;
    entry.name.assign(name);
    entry.context = std::make_unique<SignalContext>(SignalContext{this, id, std::string(name)});
    subscriptions.push_back(std::move(entry));
    if (active) {
        auto &subscription = subscriptions.back();
        const SrhStatus status = card.host->signal_subscribe(
            card.host->context, card.owner, signal,
            [](void *opaque, int32_t millivolts) -> SrhStatus {
                return srz80::sdk::guard([&]() -> SrhStatus {
                auto *context = static_cast<SignalContext *>(opaque);
                if (!context || !context->vm)
                    return SRH_INVALID;
                std::string error;
                if (!context->vm->invoke_callback(context->callback_id, "signal",
                                                  context->name, millivolts, error))
                    context->vm->log_error(context->name, error);
                return SRH_OK;
                });
            },
            subscription.context.get(), &subscription.subscription);
        if (status != SRH_OK) {
            release_function(id);
            subscriptions.pop_back();
            error = "Cannot subscribe to signal " + std::string(name);
            return false;
        }
    }
    return true;
}

bool ScriptVm::activate(std::string &error) {
    if (active)
        return true;
    if (!card.host || !card.host->schedule || !card.host->cancel ||
        !card.host->signal_subscribe) {
        error = "Host does not provide script timer and signal callbacks";
        return false;
    }
    active = true;
    // Registrations made by top-level script code are staged until evaluation
    // succeeds, then attached to the owner. At this point all definitions are
    // already recorded; setting active before callbacks lets APIs called by
    // on_reset attach new registrations immediately.
    for (auto &signal : subscriptions) {
        if (signal.subscription)
            continue;
        const SrhStatus status = card.host->signal_subscribe(
            card.host->context, card.owner, signal.signal,
            [](void *opaque, int32_t millivolts) -> SrhStatus {
                return srz80::sdk::guard([&]() -> SrhStatus {
                auto *context = static_cast<SignalContext *>(opaque);
                if (!context || !context->vm)
                    return SRH_INVALID;
                std::string error;
                if (!context->vm->invoke_callback(context->callback_id, "signal",
                                                  context->name, millivolts, error))
                    context->vm->log_error(context->name, error);
                return SRH_OK;
                });
            },
            signal.context.get(), &signal.subscription);
        if (status != SRH_OK) {
            error = "Cannot subscribe to signal " + signal.name;
            deactivate();
            return false;
        }
    }
    // Timers registered during initialization are armed only after all signal
    // subscriptions succeeded.
    for (auto &timer : timers) {
        if (timer.handle)
            continue;
        const SrhStatus status = card.host->schedule(
            card.host->context, card.owner, timer.delay_ns,
            [](void *opaque) -> SrhStatus {
                return srz80::sdk::guard([&]() -> SrhStatus {
                auto *context = static_cast<TimerContext *>(opaque);
                return context && context->vm ? context->vm->timer_fired(context) : SRH_INVALID;
                });
            },
            timer.context.get(), &timer.handle);
        if (status != SRH_OK) {
            error = "Cannot schedule script timer";
            deactivate();
            return false;
        }
    }
    return true;
}

} // namespace srz80_script
