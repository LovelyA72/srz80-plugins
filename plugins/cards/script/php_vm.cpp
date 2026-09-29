#include "script_card.h"
#include "php/php_bridge.h"
#include <dlfcn.h>
#include <link.h>
#include <mutex>

namespace srz80_script {
namespace {

// The bridge and libphp live beside the plugin, not at an absolute build path.
const char php_library_anchor = 0;

struct PhpEngine {
    void *library = nullptr;
    PhpRun run = nullptr;
    bool leased = false;
    bool reusable = true;

    ~PhpEngine() {
        if (run) {
            PhpMessage message{};
            message.operation = PHP_SHUTDOWN;
            PhpReply reply{};
            run(&message, &reply, nullptr, nullptr);
        }
        if (library) dlclose(library);
    }
};

// dlclose does not reliably reclaim libc's static TLS slots when another
// namespace remains alive. Keep engine libraries loaded, and recycle clean
// PHP requests instead. The bound also prevents growth after failed cleanup.
class PhpEnginePool {
    static constexpr size_t capacity = 8;
    std::mutex mutex;
    std::vector<std::unique_ptr<PhpEngine>> engines;
public:
    PhpEngine *acquire(std::string &error) {
        std::lock_guard lock(mutex);
        for (auto &engine : engines) {
            if (!engine->leased && engine->reusable) {
                engine->leased = true;
                return engine.get();
            }
        }
        if (engines.size() >= capacity) {
            error = "PHP engine pool exhausted (8 slots; reload needs a spare slot)";
            return nullptr;
        }
        Dl_info info{};
        if (!dladdr(&php_library_anchor, &info) || !info.dli_fname) {
            error = "Cannot locate PHP bridge beside the script plugin";
            return nullptr;
        }
        const auto path = fs::absolute(info.dli_fname).parent_path() / "php/libscript_php_bridge.so";
        auto engine = std::make_unique<PhpEngine>();
        engine->library = dlmopen(LM_ID_NEWLM, path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!engine->library) {
            error = "Cannot allocate a PHP engine (linker/TLS capacity): " + std::string(dlerror());
            return nullptr;
        }
        const auto get_api = reinterpret_cast<PhpGetApi>(dlsym(engine->library, "script_php_get_api"));
        const auto *api = get_api ? get_api() : nullptr;
        if (!api || api->abi_version != SCRIPT_PHP_BRIDGE_ABI ||
            api->struct_size < sizeof(PhpBridgeApi) || !api->run) {
            error = "PHP bridge ABI mismatch; install the matching plugin and php/ libraries together";
            return nullptr;
        }
        engine->run = api->run;
        engine->leased = true;
        auto *result = engine.get();
        engines.push_back(std::move(engine));
        return result;
    }

    void release(PhpEngine *engine, bool reusable) {
        std::lock_guard lock(mutex);
        engine->reusable = reusable;
        engine->leased = false;
    }
};

PhpEnginePool &engine_pool() {
    static PhpEnginePool pool;
    return pool;
}

struct PhpVm final : ScriptVm {
    PhpEngine *engine = nullptr;
    PhpRun run = nullptr;
    std::string host_bytes;

    PhpVm(ScriptCard &c, std::string path) : ScriptVm(c, Backend::php, std::move(path)) {}
    ~PhpVm() override {
        deactivate();
        if (engine) {
            PhpMessage message{};
            message.operation = PHP_STOP;
            PhpReply reply{};
            const bool reusable = run(&message, &reply, host_call, this) != 0;
            engine_pool().release(engine, reusable);
        }
    }

    bool execute(const PhpMessage &message, PhpReply &reply, std::string &error) {
        if (!run) {
            error = "PHP runtime unavailable";
            return false;
        }
        const bool ok = run(&message, &reply, host_call, this) != 0;
        if (!ok)
            error = reply.error;
        return ok;
    }

    bool initialize(std::string &error) override {
        engine = engine_pool().acquire(error);
        if (!engine) return false;
        run = engine->run;
        current_source = main_relative;
        const auto &source = source_cache.at(main_relative);
        PhpMessage message{};
        message.operation = PHP_LOAD;
        message.text = main_relative.c_str();
        message.text_size = main_relative.size();
        message.data = source.data();
        message.data_size = source.size();
        PhpReply reply{};
        return execute(message, reply, error);
    }

    bool call_reset(bool cold, std::string &error) override {
        PhpMessage message{};
        message.operation = PHP_RESET;
        message.a = cold;
        PhpReply reply{};
        return execute(message, reply, error);
    }
    bool call_read(uint64_t address, uint8_t &value, std::string &error) override {
        if (address > INT64_MAX) { error = "PHP address exceeds signed 64-bit range"; return false; }
        PhpMessage message{};
        message.operation = PHP_READ;
        message.a = static_cast<int64_t>(address);
        PhpReply reply{};
        if (!execute(message, reply, error)) return false;
        value = static_cast<uint8_t>(reply.value);
        return true;
    }
    bool call_write(uint64_t address, uint8_t value, std::string &error) override {
        if (address > INT64_MAX) { error = "PHP address exceeds signed 64-bit range"; return false; }
        PhpMessage message{};
        message.operation = PHP_WRITE;
        message.a = static_cast<int64_t>(address);
        message.b = value;
        PhpReply reply{};
        return execute(message, reply, error);
    }
    bool invoke_callback(uint64_t id, std::string_view kind, std::string_view name,
                         int64_t value, std::string &error) override {
        PhpMessage message{};
        message.operation = kind == "signal" ? PHP_SIGNAL : PHP_TIMER;
        message.a = static_cast<int64_t>(id);
        message.b = value;
        message.text = name.data();
        message.text_size = name.size();
        PhpReply reply{};
        return execute(message, reply, error);
    }
    bool save_user_state(std::string &saved, std::string &error) override {
        PhpMessage message{};
        message.operation = PHP_SAVE;
        PhpReply reply{};
        if (!execute(message, reply, error)) return false;
        saved.assign(reply.data, reply.size);
        return true;
    }
    bool load_user_state(std::string_view saved, std::string &error) override {
        PhpMessage message{};
        message.operation = PHP_RESTORE;
        message.data = saved.data();
        message.data_size = saved.size();
        PhpReply reply{};
        return execute(message, reply, error);
    }
    // PHP retains its values in the bridge and passes an opaque ID to the shared path.
    uint64_t retain_callback(int, JSValueConst) override { return 0; }
    void release_function(uint64_t id) override {
        PhpMessage message{};
        message.operation = PHP_RELEASE;
        message.a = static_cast<int64_t>(id);
        PhpReply reply{};
        std::string error;
        execute(message, reply, error);
    }

    static int host_call(void *context, const PhpMessage *m, PhpReply *r) noexcept {
        try {
            return static_cast<PhpVm *>(context)->host_request(*m, *r);
        } catch (const std::exception &e) {
            std::snprintf(r->error, sizeof(r->error), "%s", e.what());
        } catch (...) {
            std::snprintf(r->error, sizeof(r->error), "PHP host API failed");
        }
        return 0;
    }

    int host_request(const PhpMessage &m, PhpReply &r) {
        const auto fail = [&](std::string_view message) {
            std::snprintf(r.error, sizeof(r.error), "%.*s", static_cast<int>(message.size()), message.data());
            return 0;
        };
        const std::string_view text(m.text ? m.text : "", m.text_size);
        if (m.operation != PHP_HOST_LOG && text.find('\0') != std::string_view::npos)
            return fail("Name contains NUL");
        const auto *h = card.host;
        SrhHandle handle = 0;
        switch (m.operation) {
        case PHP_HOST_LOG:
            card.log("script [" + current_source + "]: " + std::string(text));
            return 1;
        case PHP_HOST_READ: {
            if (m.a < 0 || !card.lookup_space(text, handle)) return fail("Invalid address or address space");
            uint8_t value = 0;
            if (!h->read || h->read(h->context, card.owner, handle, m.a, &value) != SRH_OK)
                return fail("Bus read failed");
            r.value = value;
            return 1;
        }
        case PHP_HOST_WRITE:
            if (m.a < 0 || m.b < 0 || m.b > 255 || !card.lookup_space(text, handle))
                return fail("Invalid address, byte or address space");
            if (!h->write || h->write(h->context, card.owner, handle, m.a, static_cast<uint8_t>(m.b)) != SRH_OK)
                return fail("Bus write failed");
            return 1;
        case PHP_HOST_TIME: {
            uint64_t now = 0;
            if (!h->time_ns || h->time_ns(h->context, &now) != SRH_OK || now > INT64_MAX)
                return fail("Simulation time unavailable or exceeds PHP integer range");
            r.value = static_cast<int64_t>(now);
            return 1;
        }
        case PHP_HOST_SIGNAL_READ: {
            int32_t value = 0;
            if (!card.lookup_signal(text, handle) || !h->signal_read ||
                h->signal_read(h->context, handle, &value) != SRH_OK) return fail("Signal read failed");
            r.value = value;
            return 1;
        }
        case PHP_HOST_SIGNAL_DRIVE:
            if (m.a < INT32_MIN || m.a > INT32_MAX || m.b < INT32_MIN || m.b > INT32_MAX)
                return fail("Signal value exceeds 32-bit range");
            if (!card.lookup_signal(text, handle) || !h->signal_drive ||
                h->signal_drive(h->context, card.owner, handle, static_cast<int32_t>(m.a),
                                static_cast<int32_t>(m.b)) != SRH_OK) return fail("Signal drive failed");
            return 1;
        case PHP_HOST_AFTER:
            if (m.a < 0) return fail("Timer delay must be non-negative");
            if (!register_retained_timer(static_cast<uint64_t>(m.a), static_cast<uint64_t>(m.b)))
                return fail("Cannot schedule timer");
            return 1;
        case PHP_HOST_ON_SIGNAL: {
            std::string error;
            if (!register_retained_signal(text, static_cast<uint64_t>(m.b), error)) return fail(error);
            return 1;
        }
        case PHP_HOST_PROJECT_READ: {
            std::vector<uint8_t> bytes;
            auto status = card.read_project(text, bytes);
            if (status != SRH_OK) return fail(status_text(status));
            host_bytes.assign(bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data()), bytes.size());
            std::string normalized;
            if (normalize_project_path(text, normalized))
                file_snapshots[normalized] = file_fingerprint(host_bytes);
            r.data = host_bytes.data();
            r.size = host_bytes.size();
            return 1;
        }
        case PHP_HOST_PROJECT_WRITE: {
            auto status = card.write_project(text, reinterpret_cast<const uint8_t *>(m.data), m.data_size);
            return status == SRH_OK ? 1 : fail(status_text(status));
        }
        default: return fail("Unknown PHP host operation");
        }
    }
};
} // namespace

std::unique_ptr<ScriptVm> make_php_vm(ScriptCard &card, std::string path) {
    return std::make_unique<PhpVm>(card, std::move(path));
}
} // namespace srz80_script
