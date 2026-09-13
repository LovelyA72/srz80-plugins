#include <srz80/providers.h>
#include "engine.hpp"
#include "publication.hpp"
#include <boundary.hpp>
#include <cstring>
#include <memory>
namespace {
using namespace srz80::patchbay;
struct Card {
    const ShouryoHost *host = nullptr;
    const SrhHostProvidersV1 *providers = nullptr;
    Engine engine;
    uint64_t base = 0;
    SrhHandle owner = 0, timer = 0;
    uint64_t armed_deadline = UINT64_MAX;
    Publication publication;
    void invalidate() { publication.invalidate(); }
    static SrhStatus SRH_CALL wake(void *context) {
        return srz80::sdk::guard([&]() -> SrhStatus {
            auto &c = *static_cast<Card *>(context);
            c.timer = 0; c.armed_deadline = UINT64_MAX;
            c.engine.advance(c.now());
            c.arm();
            return SRH_OK;
        });
    }
    void arm() {
        const auto due = engine.next_deadline();
        if (due == armed_deadline) return;
        if (timer) host->cancel(host->context, timer);
        timer = 0; armed_deadline = UINT64_MAX;
        if (due != UINT64_MAX) {
            const auto time = now();
            if (host->schedule(host->context, owner, due > time ? due - time : 0, wake, this, &timer) != SRH_OK)
                throw std::runtime_error("cannot schedule patchbay device");
            armed_deadline = due;
        }
    }
    uint64_t now() const {
        uint64_t n = 0;
        providers->simulation_time_ns(providers->context, &n);
        return n;
    }
};
SrhStatus copy(const std::string &s, char *buffer, uint64_t *size) {
    if (!size || s.size() >= SRH_PROVIDER_MAX_BYTES)
        return SRH_INVALID;
    const auto capacity = *size;
    *size = s.size() + 1;
    if (!buffer)
        return SRH_OK;
    if (capacity < *size)
        return SRH_INVALID;
    std::memcpy(buffer, s.c_str(), static_cast<size_t>(*size));
    return SRH_OK;
}
SrhStatus SRH_CALL snapshot(void *p, char *buffer, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        auto &c = *static_cast<Card *>(p);
        if (!size) return SRH_INVALID;
        return copy(c.publication.snapshot(c.engine, c.now(), !buffer), buffer, size);
    });
}
SrhStatus SRH_CALL save_project(void *p, uint8_t *buffer, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!p || !size) return SRH_INVALID;
        const auto text = static_cast<Card *>(p)->engine.topology.dump();
        const auto capacity = *size; *size = text.size();
        if (!buffer) return SRH_OK;
        if (capacity < *size) return SRH_INVALID;
        std::memcpy(buffer, text.data(), text.size()); return SRH_OK;
    });
}
SrhStatus SRH_CALL load_project(void *p, const uint8_t *buffer, uint64_t size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!p || !buffer || !size || size >= SRH_PROVIDER_MAX_BYTES) return SRH_INVALID;
        auto &c = *static_cast<Card *>(p);
        c.engine.replace(Json::parse(buffer, buffer+size), c.now());
        c.arm(); c.invalidate();
        return SRH_OK;
    });
}
SrhStatus SRH_CALL command(void *p, uint32_t kind, uint64_t revision, const char *text, uint64_t size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        auto &c = *static_cast<Card *>(p);
        if (!text || !size || size >= SRH_PROVIDER_MAX_BYTES || kind > 1)
            return SRH_INVALID;
        if (revision != c.engine.revision)
            return SRH_CONFLICT;
        const auto j = Json::parse(text, text + size);
        // Validate and settle on a copy: a rejected command never changes state.
        auto candidate = c.engine;
        if (kind == 1)
            candidate.replace(j, c.now(), true);
        else
            candidate.interact(j, c.now());
        c.engine = std::move(candidate);
        c.arm(); c.invalidate();
        return SRH_OK;
    });
}
SrhStatus SRH_CALL read(void *p, uint64_t address, uint8_t *value) {
    if (!value)
        return SRH_INVALID;
    auto &c = *static_cast<Card *>(p);
    if (address < c.base || address - c.base >= 6)
        return SRH_INVALID;
    const auto &e = c.engine;
    const uint8_t regs[] = {e.input, e.output, e.direction, e.defaults, e.change, e.conflict};
    *value = regs[address - c.base];
    return SRH_OK;
}
SrhStatus SRH_CALL write(void *p, uint64_t address, uint8_t value) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        auto &c = *static_cast<Card *>(p);
        if (address < c.base || address - c.base >= 6)
            return SRH_INVALID;
        auto &next = c.engine;
        switch (address - c.base) {
        case 1:
            next.output = value;
            break;
        case 2:
            next.direction = value;
            break;
        case 3:
            next.defaults = value;
            break;
        case 4:
            next.change &= static_cast<uint8_t>(~value);
            return SRH_OK;
        default:
            return SRH_OK;
        }
        next.propagate(c.now());
        c.arm();
        return SRH_OK;
    });
}
SrhStatus SRH_CALL reset(void *p, uint32_t) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        auto &c = *static_cast<Card *>(p);
        Engine next;
        next.replace(c.engine.topology, c.now());
        next.revision = c.engine.revision;
        next.runtime_revision = c.engine.runtime_revision + 1;
        c.engine = std::move(next);
        // Core may already have cancelled scheduled events during reset.
        if (c.timer) c.host->cancel(c.host->context, c.timer);
        c.timer = 0; c.armed_deadline = UINT64_MAX;
        c.arm(); c.invalidate();
        return SRH_OK;
    });
}
SrhStatus SRH_CALL save(void *p, uint8_t *buffer, uint64_t *size) {
    return srz80::sdk::guard([&] {
        return copy(static_cast<Card *>(p)->engine.save().dump(), reinterpret_cast<char *>(buffer), size);
    });
}
SrhStatus SRH_CALL load(void *p, const uint8_t *buffer, uint64_t size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!buffer || !size || size > SRH_PROVIDER_MAX_BYTES || buffer[size - 1] != 0)
            return SRH_INVALID;
        auto &c = *static_cast<Card *>(p);
        c.engine.restore(Json::parse(buffer, buffer + size - 1));
        if (c.timer) c.host->cancel(c.host->context, c.timer);
        c.timer = 0; c.armed_deadline = UINT64_MAX;
        c.arm(); c.invalidate();
        return SRH_OK;
    });
}
SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !out ||
            !srz80::sdk::has_field(host, &ShouryoHost::query) || !host->query ||
            config->base > UINT64_MAX - 5)
            return SRH_INVALID;
        const void *extension = nullptr;
        if (host->query(host->context, "host.providers.v1", &extension) != SRH_OK)
            return SRH_UNAVAILABLE;
        const auto *providers = static_cast<const SrhHostProvidersV1 *>(extension);
        if (!srz80::sdk::valid(providers) || !providers->register_provider || !providers->simulation_time_ns)
            return SRH_INVALID;
        auto c = std::make_unique<Card>();
        c->host = host;
        c->providers = providers;
        c->base = config->base;
        c->owner = owner;
        c->engine.replace(empty_topology(), c->now());
        SrhMapping mapping{SRH_INIT(SrhMapping),
                           config->space,
                           config->base,
                           config->base + 5,
                           config->priority,
                           c.get(),
                           read,
                           write,
                           read,
                           nullptr};
        SrhHandle handle = 0;
        if (host->map(host->context, owner, &mapping, &handle) != SRH_OK)
            return SRH_ERROR;
        SrhDataProviderV1 provider{SRH_INIT(SrhDataProviderV1), c.get(), snapshot, command, "Patchbay GPIO", {}, SRH_PROVIDER_LIVE_CONFIG};
        std::strcpy(provider.protocol, protocol_id);
        if (providers->register_provider(providers->context, owner, &provider) != SRH_OK)
            return SRH_ERROR;
        *out = c.release();
        return SRH_OK;
    });
}
void SRH_CALL destroy(void *p) {
    delete static_cast<Card *>(p);
}
const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor),
                                   "I/O",
                                   "Logical GPIO",
                                   "8-bit GPIO and logical component patchbay",
                                   0x40,
                                   6,
                                   0,
                                   0,
                                   0,
                                   0,
                                   "{}",
                                   nullptr,
                                   nullptr};
const SrhPlugin api{SRH_INIT(SrhPlugin),
                    "patchbay",
                    create,
                    destroy,
                    reset,
                    [](void *) -> uint32_t { return 0; },
                    [](void *, uint32_t, SrhProperty *) -> SrhStatus { return SRH_NOT_FOUND; },
                    [](void *, uint32_t, SrhValue *) -> SrhStatus { return SRH_NOT_FOUND; },
                    [](void *, uint32_t, const SrhValue *) -> SrhStatus { return SRH_NOT_FOUND; },
                    save,
                    load,
                    &descriptor, save_project, load_project};
} // namespace
extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *) {
    return &api;
}
