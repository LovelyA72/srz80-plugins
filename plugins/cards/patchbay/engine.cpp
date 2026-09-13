#include "engine.hpp"
namespace srz80::patchbay {
void Observer::advance(uint64_t now) {
    if (now < last)
        throw std::runtime_error("non-monotonic observation time");
    uint64_t elapsed = now - last;
    const uint64_t left = window - (last - start);
    if (elapsed >= left) {
        high += on ? left : 0;
        duty = static_cast<uint32_t>(high * 65535 / window);
        stable = true;
        elapsed -= left;
        start = last + left;
        high = 0;
        if (elapsed >= window) {
            duty = on ? 65535 : 0;
            start += elapsed / window * window;
            elapsed %= window;
        }
    }
    high += on ? elapsed : 0;
    last = now;
}

uint32_t Observer::brightness(uint64_t now) const {
    auto copy = *this;
    copy.advance(now);
    return copy.stable ? copy.duty
                       : (now > copy.start ? static_cast<uint32_t>(copy.high * 65535 / (now - copy.start))
                                           : (on ? 65535 : 0));
}

Json Observer::save() const {
    return {start, last, high, window, duty, on, stable};
}

void Observer::load(const Json &j) {
    start = j.at(0);
    last = j.at(1);
    high = j.at(2);
    window = j.at(3);
    duty = j.at(4);
    on = j.at(5);
    stable = j.at(6);
    if (window < 1000 || window > 1000000000 || last < start || last - start >= window ||
        high > last - start || duty > 65535)
        throw std::runtime_error("invalid PWM state");
}

void Engine::require(bool value) {
    if (!value)
        throw std::runtime_error("invalid patchbay topology/state");
}

void Engine::replace(Json document, uint64_t now, bool preserve_state) {
    // The editor sends complete documents.  Common wire/configuration edits
    // can nevertheless retain the executable endpoint graph and settle only
    // the affected region.
    if (preserve_state && patch_topology(document, now)) {
        revision += 1;
        return;
    }
    require(document.is_object() && document.at("schema") == 1 && document.at("components").is_array() &&
            document.at("nets").is_array());
    require(document["components"].size() <= 128 && document["nets"].size() <= 512);
    Engine next(resolver_);
    next.output = output;
    next.direction = direction;
    next.defaults = defaults;
    const auto types = catalogue();
    std::set<std::string> endpoints;
    for (int i = 0; i < 8; ++i)
        endpoints.insert("gpio.P" + std::to_string(i));
    for (auto &def : document["components"]) {
        std::string id = def.at("id");
        require(!id.empty() && id.size() <= 64 && id != "gpio" &&
                id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
                    std::string::npos &&
                !next.components.contains(id));
        auto it = std::find_if(types.begin(), types.end(),
                               [&](const Json &d) { return d["type"] == def.at("type"); });
        require(it != types.end() && def.at("schema") == 1);
        if (!def.contains("config"))
            def["config"] = Json::object();
        require(def["config"].is_object());
        for (auto &[key, val] : def["config"].items()) {
            require((*it)["properties"].contains(key));
            const auto prop = (*it)["properties"][key];
            if (prop["type"] == "boolean")
                require(val.is_boolean());
            else if (prop["type"] == "integer")
                require((val.is_number_integer() || val.is_number_unsigned()) &&
                        val.get<int64_t>() >= prop["min"].get<int64_t>() &&
                        val.get<int64_t>() <= prop["max"].get<int64_t>());
            else if (prop["type"] == "color")
                require((val.is_number_integer() || val.is_number_unsigned()) && val.get<int64_t>() >= 0 &&
                        val.get<uint64_t>() <= 0xffffff);
            else
                require(false);
        }
        Component c;
        c.definition = def;
        c.descriptor = *it;
        for (const auto &p : (*it)["pins"]) {
            std::string name = p["name"];
            endpoints.insert(id + "." + name);
            c.pins[name] = Floating;
        }
        const std::string renderer = (*it)["renderer"];
        if (renderer == "led" || renderer == "rgb" || renderer == "segment7" || renderer == "segment11") {
            c.observers.resize(c.pins.size());
            for (auto &o : c.observers) {
                o.start = o.last = now;
                o.window = def["config"].value("window_ns", uint64_t{20000000});
            }
        }
        if (preserve_state) {
            auto old = components.find(id);
            if (old != components.end() && old->second.definition["type"] == def["type"] &&
                old->second.definition["schema"] == def["schema"]) {
                auto observers = std::move(c.observers);
                c = old->second;
                // Reconfigure only observations whose polarity/window changed.
                if (c.definition["config"] != def["config"]) c.observers = std::move(observers);
                c.definition = def;
            }
        }
        next.components.emplace(id, std::move(c));
    }
    std::set<std::string> used, net_ids;
    for (auto &net : document["nets"]) {
        std::string id = net.at("id");
        require(!id.empty() && id.size() <= 64 && net_ids.insert(id).second);
        require(net.at("pins").is_array() && net["pins"].size() >= 2 && net["pins"].size() <= 256);
        int fallback = net.value("default", 2);
        require(fallback >= 0 && fallback <= 2);
        for (const auto &pin : net["pins"]) {
            std::string key = pin;
            require(endpoints.contains(key) && used.insert(key).second);
        }
    }
    next.topology = std::move(document);
    next.revision = revision + 1;
    next.runtime_revision = runtime_revision + 1;
    if (preserve_state) {
        next.pins = pins;
        next.input = input;
        next.change = change;
        next.conflict = conflict;
    }
    next.compile();
    next.propagate(now);
    if (!preserve_state) next.change = 0;
    *this = std::move(next);
}

bool Engine::patch_topology(const Json &document, uint64_t now) {
    try {
        if (!document.is_object() || document.at("schema") != 1 || !document.at("components").is_array() ||
            !document.at("nets").is_array() || document["components"].size() != components.size() ||
            document["nets"].size() > 512)
            return false;

        const auto types = catalogue();
        std::set<std::string> seen;
        std::vector<size_t> changed_devices;
        for (auto &def : document["components"]) {
            const std::string id = def.at("id");
            auto current = components.find(id);
            if (current == components.end() || !seen.insert(id).second || def.at("type") != current->second.definition.at("type") ||
                def.at("schema") != current->second.definition.at("schema"))
                return false;
            auto descriptor = std::find_if(types.begin(), types.end(),
                                           [&](const Json &d) { return d["type"] == def.at("type"); });
            if (descriptor == types.end()) return false;
            const Json config = def.value("config", Json::object());
            if (!config.is_object()) return false;
            for (const auto &[key, val] : config.items()) {
                if (!(*descriptor)["properties"].contains(key)) return false;
                const auto prop = (*descriptor)["properties"][key];
                if (prop["type"] == "boolean") { if (!val.is_boolean()) return false; }
                else if (prop["type"] == "integer") {
                    if (!(val.is_number_integer() || val.is_number_unsigned()) || val.get<int64_t>() < prop["min"].get<int64_t>() ||
                        val.get<int64_t>() > prop["max"].get<int64_t>()) return false;
                } else if (prop["type"] == "color") {
                    if (!(val.is_number_integer() || val.is_number_unsigned()) || val.get<int64_t>() < 0 ||
                        val.get<uint64_t>() > 0xffffff) return false;
                } else return false;
            }
            if (current->second.definition["config"] != config) {
                const std::string renderer = current->second.descriptor["renderer"];
                if (renderer == "led" || renderer == "rgb" || renderer == "segment7" || renderer == "segment11") {
                    current->second.observers.resize(current->second.pins.size());
                    for (auto &o : current->second.observers) {
                        o.start = o.last = now;
                        o.window = config.value("window_ns", uint64_t{20000000});
                    }
                }
                current->second.definition = def;
                current->second.definition["config"] = config;
                auto device = std::find_if(devices_.begin(), devices_.end(), [&](const Device &d) { return d.id == id; });
                changed_devices.push_back(static_cast<size_t>(device - devices_.begin()));
            } else {
                current->second.definition = def;
                current->second.definition["config"] = config;
            }
        }

        std::map<std::string, size_t> ids;
        for (size_t e = 0; e < endpoints_.size(); ++e) ids.emplace(endpoints_[e].key, e);
        std::set<std::string> used, net_ids;
        std::vector<Net> next_nets;
        std::vector<size_t> next_net(endpoints_.size(), SIZE_MAX);
        for (const auto &net : document["nets"]) {
            const std::string id = net.at("id");
            if (id.empty() || id.size() > 64 || !net_ids.insert(id).second || !net.at("pins").is_array() ||
                net["pins"].size() < 2 || net["pins"].size() > 256) return false;
            const int fallback = net.value("default", Floating);
            if (fallback < Low || fallback > Conflict) return false;
            Net compiled{{}, fallback, false};
            for (const auto &pin : net["pins"]) {
                const std::string key = pin;
                auto endpoint = ids.find(key);
                if (endpoint == ids.end() || !used.insert(key).second) return false;
                compiled.endpoints.push_back(endpoint->second);
            }
            next_nets.push_back(std::move(compiled));
        }
        // Fill explicit-net memberships before assigning singleton nets.
        for (size_t n = 0; n < next_nets.size(); ++n)
            for (auto endpoint : next_nets[n].endpoints) next_net[endpoint] = n;
        for (size_t e = 0; e < endpoints_.size(); ++e)
            if (next_net[e] == SIZE_MAX) { next_net[e] = next_nets.size(); next_nets.push_back({{e}, Floating, false}); }

        auto signature = [](const Net &net) { auto result = net.endpoints; std::sort(result.begin(), result.end()); return result; };
        std::map<std::vector<size_t>, int> old_nets, new_nets;
        for (size_t n = 0; n < nets_.size(); ++n) old_nets[signature(nets_[n])] = nets_[n].fallback;
        for (size_t n = 0; n < next_nets.size(); ++n) new_nets[signature(next_nets[n])] = next_nets[n].fallback;
        nets_ = std::move(next_nets);
        dirty_nets_.clear();
        dirty_nets_.reserve(nets_.size());
        for (size_t n = 0; n < nets_.size(); ++n)
            if (!old_nets.contains(signature(nets_[n])) || old_nets[signature(nets_[n])] != nets_[n].fallback) dirty_net(n);
        for (size_t e = 0; e < endpoints_.size(); ++e) endpoints_[e].net = next_net[e];
        for (auto id : changed_devices) { drive_device(id); dirty_device(id); }
        topology = document;
        propagate(now);
        return true;
    } catch (...) {
        return false;
    }
}

Engine::Engine(const Engine &other)
    : last_work(other.last_work), topology(other.topology), components(other.components), pins(other.pins),
      revision(other.revision), runtime_revision(other.runtime_revision), output(other.output),
      direction(other.direction), defaults(other.defaults), change(other.change), input(other.input),
      conflict(other.conflict), endpoints_(other.endpoints_), nets_(other.nets_), devices_(other.devices_),
      dirty_nets_(other.dirty_nets_), dirty_devices_(other.dirty_devices_), timers_(other.timers_), resolver_(other.resolver_) {
    bind();
}
Engine &Engine::operator=(const Engine &other) {
    if (this != &other) { Engine copy(other); *this = std::move(copy); }
    return *this;
}
void Engine::bind() {
    dirty_nets_.reserve(nets_.size()); dirty_devices_.reserve(devices_.size());
    for (auto &device : devices_) {
        device.state = &components.at(device.id);
        device.implementation = &resolver_(*device.state);
    }
    for (auto &p : endpoints_) {
        p.published = &pins.at(p.key);
        p.local = p.component == SIZE_MAX ? nullptr : &devices_[p.component].state->pins.at(p.pin);
    }
}
void Engine::sync_timer(size_t id) {
    auto &device = devices_[id];
    const auto due = device.state->deadline;
    require(due == UINT64_MAX || device.implementation->timer);
    if (due == device.scheduled) return;
    if (device.scheduled != UINT64_MAX) timers_.erase({device.scheduled, id});
    device.scheduled = due;
    if (due != UINT64_MAX) timers_.insert({due, id});
}
void Engine::dirty_net(size_t id) {
    if (!nets_[id].dirty) { nets_[id].dirty = true; dirty_nets_.push_back(id); }
}
void Engine::dirty_device(size_t id) {
    if (!devices_[id].dirty) { devices_[id].dirty = true; dirty_devices_.push_back(id); }
}
void Engine::drive_device(size_t id) {
    const auto &c = *devices_[id].state;
    const auto &m = *devices_[id].implementation;
    for (auto e : devices_[id].endpoints) {
        auto &p = endpoints_[e];
        int value = m.drive(c, p.pin);
        if (p.drive != value) { p.drive = value; dirty_net(p.net); }
    }
}
void Engine::compile() {
    endpoints_.clear(); nets_.clear(); devices_.clear();
    dirty_nets_.clear(); dirty_devices_.clear(); timers_.clear();
    std::map<std::string, size_t> ids;
    auto add = [&](std::string key, std::string pin, size_t device) {
        size_t id = endpoints_.size();
        ids[key] = id;
        int level = pins.contains(key) ? pins.at(key) : Floating;
        endpoints_.push_back({key, pin, device, SIZE_MAX, Floating, level});
    };
    for (int i = 0; i < 8; ++i) add("gpio.P" + std::to_string(i), "", SIZE_MAX);
    for (const auto &[id, c] : components) {
        size_t device = devices_.size();
        devices_.push_back({id, {}, false});
        for (const auto &p : c.descriptor["pins"]) {
            std::string name = p["name"];
            devices_.back().endpoints.push_back(endpoints_.size());
            add(id + "." + name, name, device);
        }
    }
    for (const auto &net : topology["nets"]) {
        size_t id = nets_.size();
        nets_.push_back({{}, net.value("default", Floating), false});
        for (const auto &key : net["pins"]) {
            size_t e = ids.at(key.get<std::string>());
            nets_.back().endpoints.push_back(e); endpoints_[e].net = id;
        }
    }
    // Unconnected endpoints are singleton nets, including their own driver.
    pins.clear();
    for (size_t e = 0; e < endpoints_.size(); ++e) {
        auto &p = endpoints_[e];
        if (p.net == SIZE_MAX) { p.net = nets_.size(); nets_.push_back({{e}, Floating, false}); }
        pins[p.key] = p.level;
    }
    dirty_nets_.reserve(nets_.size()); dirty_devices_.reserve(devices_.size());
    bind();
    for (size_t i = 0; i < devices_.size(); ++i) sync_timer(i);
    for (size_t i = 0; i < nets_.size(); ++i) dirty_net(i);
    for (size_t i = 0; i < devices_.size(); ++i) { drive_device(i); dirty_device(i); }
}
void Engine::propagate(uint64_t now) {
    if (endpoints_.empty()) compile();
    last_work = {};
    std::array<int, 8> previous{};
    for (size_t i = 0; i < 8; ++i) {
        auto &p = endpoints_[i]; previous[i] = p.level;
        int value = (direction & (1u << i)) ? ((output >> i) & 1) : Floating;
        if (p.drive != value) { p.drive = value; dirty_net(p.net); }
    }
    // Resolve a complete wave before observing edges; all devices sample the
    // pre-update outputs, including cascaded shift registers sharing a clock.
    size_t waves = 0;
    while (!dirty_nets_.empty() || !dirty_devices_.empty()) {
        require(waves++ <= components.size() + 2);
        for (auto id : dirty_nets_) {
            auto &net = nets_[id]; net.dirty = false; ++last_work.nets;
            int level = Floating;
            for (auto e : net.endpoints) {
                int d = endpoints_[e].drive;
                if (d != Floating) level = level == Floating ? d : (level == d ? level : Conflict);
            }
            if (level == Floating) level = net.fallback;
            for (auto e : net.endpoints) {
                auto &p = endpoints_[e];
                if (p.level == level) continue;
                p.level = level; *p.published = level;
                if (p.component != SIZE_MAX) {
                    *p.local = level;
                    dirty_device(p.component);
                }
            }
        }
        dirty_nets_.clear();
        for (auto id : dirty_devices_) {
            auto &c = *devices_[id].state; ++last_work.devices;
            const auto &m = *devices_[id].implementation;
            if (m.update) m.update(c, now);
            require(c.deadline == UINT64_MAX || c.deadline > now);
            sync_timer(id);
            const int active = c.active() ? High : Low;
            for (size_t i = 0; i < c.observers.size(); ++i) {
                auto &o = c.observers[i]; o.advance(now);
                o.on = endpoints_[devices_[id].endpoints[i]].level == active;
            }
        }
        for (auto id : dirty_devices_) { devices_[id].dirty = false; drive_device(id); }
        dirty_devices_.clear();
    }
    uint8_t next = 0; conflict = 0;
    for (size_t i = 0; i < 8; ++i) {
        int v = endpoints_[i].level;
        if (previous[i] != v) change |= static_cast<uint8_t>(1u << i);
        if (v == High || (v == Floating && (defaults & (1u << i)))) next |= static_cast<uint8_t>(1u << i);
        if (v == Conflict) conflict |= static_cast<uint8_t>(1u << i);
    }
    change |= input ^ next; input = next; ++runtime_revision;
}
uint64_t Engine::next_deadline() const {
    return timers_.empty() ? UINT64_MAX : timers_.begin()->first;
}
void Engine::advance(uint64_t now) {
    for (uint64_t due = next_deadline(); due <= now && due != UINT64_MAX; due = next_deadline()) {
        // Run all equal-time callbacks before publishing any changed drives.
        while (!timers_.empty() && timers_.begin()->first == due) {
            size_t id = timers_.begin()->second;
            auto &device = devices_[id];
            auto &c = *device.state;
            c.deadline = UINT64_MAX;
            if (device.implementation->timer) device.implementation->timer(c, due);
            require(c.deadline == UINT64_MAX || c.deadline > due);
            sync_timer(id);
            dirty_device(id);
        }
        // Timer outputs must be visible in the first resolution wave.
        for (auto id : dirty_devices_) drive_device(id);
        propagate(due);
    }
}

void Engine::interact(const Json &j, uint64_t now) {
    const std::string id = j.at("id");
    require(components.contains(id));
    auto &c = components.at(id);
    const int maximum = resolver_(c).max_value;
    require(maximum >= 0);
    require(j.at("value").is_number_integer());
    int value = j.at("value");
    require(value >= 0 && value <= maximum);
    c.value = static_cast<uint32_t>(value);
    for (size_t i = 0; i < devices_.size(); ++i)
        if (devices_[i].id == id) { drive_device(i); dirty_device(i); break; }
    propagate(now);
}

Json Engine::snapshot(uint64_t now) const {
    Json runtime = Json::object();
    for (const auto &[id, c] : components) {
        Json brightness = Json::array(), observations = Json::array();
        for (const auto &o : c.observers) {
            brightness.push_back(o.brightness(now));
            auto copy = o;
            copy.advance(now);
            observations.push_back({{"on", o.on},
                                    {"stable", copy.stable},
                                    {"interval_ns", copy.stable ? copy.window : now - copy.start}});
        }
        runtime[id] = {{"pins", c.pins},   {"brightness", brightness}, {"observations", observations},
                       {"value", c.value}, {"shift", c.shift},         {"latch", c.latch}};
    }
    return {{"protocol", 1},
            {"catalogue_version", 1},
            {"catalogue", catalogue()},
            {"revision", revision},
            {"runtime_revision", runtime_revision},
            {"topology", topology},
            {"runtime", runtime},
            {"pins", pins},
            {"now_ns", now},
            {"gpio", {{"name", "GPIO"}, {"pins", {"P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7"}}}}};
}

Json Engine::save() const {
    Json states = Json::object();
    for (const auto &[id, c] : components) {
        Json obs = Json::array();
        for (const auto &o : c.observers)
            obs.push_back(o.save());
        states[id] = {{"value", c.value},
                      {"deadline", c.deadline},
                      {"shift", c.shift},
                      {"latch", c.latch},
                      {"shift_unknown", c.shift_unknown},
                      {"latch_unknown", c.latch_unknown},
                      {"clock", c.clock},
                      {"latch_clock", c.latch_clock},
                      {"observers", obs},
                      {"pins", c.pins}};
    }
    return {{"schema", 1},          {"topology", topology},
            {"components", states}, {"registers", {output, direction, defaults, change, input, conflict}},
            {"pins", pins},         {"runtime_revision", runtime_revision}};
}

void Engine::restore(const Json &j) {
    require(j.at("schema") == 1 && j.at("topology") == topology);
    Engine copy = *this;
    for (auto &[id, c] : copy.components) {
        const auto &s = j.at("components").at(id);
        c.value = s.at("value");
        if (s.contains("deadline")) {
            const auto &deadline = s.at("deadline");
            require(deadline.is_number_unsigned() ||
                    (deadline.is_number_integer() && deadline.get<int64_t>() >= 0));
        }
        c.deadline = s.value("deadline", UINT64_MAX);
        c.shift = s.at("shift");
        c.latch = s.at("latch");
        require(c.value <= 255 && c.shift <= 255 && c.latch <= 255);
        c.shift_unknown = s.at("shift_unknown");
        c.latch_unknown = s.at("latch_unknown");
        require(c.shift_unknown <= 255 && c.latch_unknown <= 255);
        c.clock = s.at("clock");
        c.latch_clock = s.at("latch_clock");
        require(c.clock >= 0 && c.clock <= 3 && c.latch_clock >= 0 && c.latch_clock <= 3);
        require(s.at("observers").size() == c.observers.size());
        for (size_t i = 0; i < c.observers.size(); ++i)
            c.observers[i].load(s["observers"][i]);
        for (auto &[name, v] : c.pins) {
            v = s.at("pins").at(name);
            require(v >= 0 && v <= 3);
        }
    }
    const auto &r = j.at("registers");
    require(r.is_array() && r.size() == 6);
    for (const auto &v : r)
        require(v.is_number_integer() && v.get<int>() >= 0 && v.get<int>() <= 255);
    copy.output = r[0];
    copy.direction = r[1];
    copy.defaults = r[2];
    copy.change = r[3];
    copy.input = r[4];
    copy.conflict = r[5];
    for (auto &[name, v] : copy.pins) {
        v = j.at("pins").at(name);
        require(v >= 0 && v <= 3);
    }
    copy.runtime_revision = j.at("runtime_revision");
    copy.compile();
    *this = std::move(copy);
}

} // namespace srz80::patchbay
