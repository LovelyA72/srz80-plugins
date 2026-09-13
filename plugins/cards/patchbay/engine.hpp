#pragma once
#include "contract.hpp"
#include <array>
#include <map>
#include <set>
#include <stdexcept>
namespace srz80::patchbay {
// Exact, tumbling simulated-time windows. Completed windows are held until
// another completes. Startup reports the partial window. Division rounds down.
struct Observer {
    uint64_t start = 0, last = 0, high = 0, window = 20000000;
    uint32_t duty = 0;
    bool on = false, stable = false;
    void advance(uint64_t now);
    uint32_t brightness(uint64_t now) const;
    Json save() const;
    void load(const Json &j);
};
struct Component {
    Json definition, descriptor;
    uint32_t value = 0, shift = 0, latch = 0, shift_unknown = 0, latch_unknown = 0;
    int clock = Floating, latch_clock = Floating;
    // One simulated-time deadline per model; UINT64_MAX means disarmed.
    uint64_t deadline = UINT64_MAX;
    std::map<std::string, int> pins;
    std::vector<Observer> observers;
    bool active() const {
        return definition.at("config").value("active_high", true);
    }
};
// Component implementations own their drive, edge and interaction semantics.
int led_drive(const Component &, const std::string &);
int rgb_led_drive(const Component &, const std::string &);
int segment7_drive(const Component &, const std::string &);
int segment11_drive(const Component &, const std::string &);
int button_drive(const Component &, const std::string &);
int switch_drive(const Component &, const std::string &);
int dip8_drive(const Component &, const std::string &);
int hc595_drive(const Component &, const std::string &);
void hc595_update(Component &, uint64_t);
struct Model {
    int (*drive)(const Component &, const std::string &);
    void (*update)(Component &, uint64_t);
    int max_value;
    // Called at the deadline, with that deadline cleared. Rearm strictly later.
    void (*timer)(Component &, uint64_t) = nullptr;
};
inline const Model &model(const Component &c) {
    static const std::map<std::string, Model> models = {
        {"led", {led_drive, nullptr, -1}},           {"rgb", {rgb_led_drive, nullptr, -1}},
        {"segment7", {segment7_drive, nullptr, -1}}, {"segment11", {segment11_drive, nullptr, -1}},
        {"button", {button_drive, nullptr, 1}},      {"switch", {switch_drive, nullptr, 1}},
        {"dip8", {dip8_drive, nullptr, 255}},        {"595", {hc595_drive, hc595_update, -1}}};
    return models.at(c.descriptor["renderer"].get<std::string>());
}
class Engine {
  public:
    // Resolvers return immutable models whose lifetime covers the engine.
    using ModelResolver = const Model &(*)(const Component &);
    explicit Engine(ModelResolver resolver = model) : resolver_(resolver) {}
    Engine(const Engine &other);
    Engine &operator=(const Engine &other);
    Engine(Engine &&) noexcept = default;
    Engine &operator=(Engine &&) noexcept = default;
    struct Work {
        size_t nets = 0, devices = 0;
    };
    Work last_work;
    Json topology = empty_topology();
    std::map<std::string, Component> components;
    std::map<std::string, int> pins;
    uint64_t revision = 1, runtime_revision = 1;
    uint8_t output = 0, direction = 0, defaults = 0, change = 0, input = 0, conflict = 0;
    static void require(bool value);
    void replace(Json document, uint64_t now, bool preserve_state = false);
    void propagate(uint64_t now);
    uint64_t next_deadline() const;
    void advance(uint64_t now);
    void interact(const Json &j, uint64_t now);
    Json snapshot(uint64_t now) const;
    Json save() const;
    void restore(const Json &j);

  private:
    struct Endpoint {
        std::string key, pin;
        size_t component, net;
        int drive = Floating, level = Floating;
        int *published = nullptr, *local = nullptr;
    };
    struct Net {
        std::vector<size_t> endpoints;
        int fallback = Floating;
        bool dirty = false;
    };
    struct Device {
        std::string id;
        std::vector<size_t> endpoints;
        bool dirty = false;
        Component *state = nullptr;
        const Model *implementation = nullptr;
        uint64_t scheduled = UINT64_MAX;
    };
    std::vector<Endpoint> endpoints_;
    std::vector<Net> nets_;
    std::vector<Device> devices_;
    std::vector<size_t> dirty_nets_, dirty_devices_;
    std::set<std::pair<uint64_t, size_t>> timers_;
    ModelResolver resolver_;
    // Rebuilt only at topology/restore boundaries; copies rebind cached pointers.
    void compile();
    void bind();
    void sync_timer(size_t id);
    void dirty_net(size_t id);
    void dirty_device(size_t id);
    void drive_device(size_t id);
    // Applies an edit without recreating endpoint/device storage when all
    // components retain their identity and type. Returns false for edits
    // which need the conservative full rebuild path.
    bool patch_topology(const Json &document, uint64_t now);
};
} // namespace srz80::patchbay
