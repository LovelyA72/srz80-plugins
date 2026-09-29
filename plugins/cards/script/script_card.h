#pragma once

#include <state.hpp>
#include <boundary.hpp>
#include <nlohmann/json.hpp>
#include <srz80/project_files.h>
#include <srz80/lifecycle.h>
#include <srz80/signals.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <quickjs.h>
#include <mruby.h>
#define PK_IS_PUBLIC_INCLUDE
#include <pocketpy/pocketpy.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace srz80_script {

using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr uint64_t kSourceLimit = 16ull * 1024 * 1024;
constexpr uint64_t kFileLimit = 16ull * 1024 * 1024;
constexpr size_t kVmMemoryLimit = 64ull * 1024 * 1024;
constexpr size_t kStateLimit = 1024ull * 1024;
constexpr int kLuaHookStep = 100;
constexpr uint64_t kLuaInstructionLimit = 10'000'000;
constexpr uint64_t kDefaultPythonInstructionLimit = 10'000'000;
constexpr uint64_t kMaxPythonInstructionLimit = 1'000'000'000;
constexpr uint64_t kQuickJsInterruptLimit = 1000;
constexpr uint32_t kQuickJsInterruptQuantum = 10'000;
constexpr size_t kCallbackDepthLimit = 64;

enum class Backend { lua, javascript, mruby, python, php };
struct ScriptCard;
struct ScriptVm;

struct TimerContext {
    ScriptVm *vm = nullptr;
    uint64_t callback_id = 0;
};
struct SignalContext {
    ScriptVm *vm = nullptr;
    uint64_t callback_id = 0;
    std::string name;
};
struct TimerEntry {
    uint64_t callback_id = 0;
    uint64_t delay_ns = 0;
    SrhHandle handle = 0;
    std::unique_ptr<TimerContext> context;
};
struct SignalEntry {
    uint64_t callback_id = 0;
    SrhHandle signal = 0;
    SrhHandle subscription = 0;
    std::string name;
    std::unique_ptr<SignalContext> context;
};

struct ScriptCard {
    const ShouryoHost *host = nullptr;
    const SrhHostProjectFilesV1 *files = nullptr;
    const SrhHostResourcesV1 *resources = nullptr;
    const SrhHostLifecycleV1 *lifecycle = nullptr;
    SrhHandle owner = 0;
    SrhHandle mapping = 0;
    uint64_t base = 0;
    uint64_t size = 0;
    fs::path project_root;
    std::string main_file;
    uint64_t python_instruction_limit = kDefaultPythonInstructionLimit;
    SrhHandle resume_subscription = 0;
    bool restart_pending = false;
    std::string main_relative;
    std::unique_ptr<ScriptVm> vm;
    std::unordered_map<std::string, SrhHandle> spaces;
    std::unordered_map<std::string, SrhHandle> signals;

    SrhStatus log(std::string_view message) const {
        if (!host || !host->log)
            return SRH_UNAVAILABLE;
        std::string text(message);
        return host->log(host->context, owner, text.c_str());
    }
    SrhStatus read_project(std::string_view relative, std::vector<uint8_t> &out);
    SrhStatus write_project(std::string_view relative, const uint8_t *data, uint64_t size);
    bool lookup_space(std::string_view name, SrhHandle &handle);
    bool lookup_signal(std::string_view name, SrhHandle &handle);
    std::unique_ptr<ScriptVm> make_vm(std::string_view path, std::string &error,
                                      bool activate = true);
    bool sources_changed();
};

// Text and project-path helpers shared across translation units.
bool valid_utf8_text(const char *data, size_t size);
std::string path_utf8(const fs::path &path);
fs::path path_from_utf8(std::string_view value);
bool path_is_within(const fs::path &root, const fs::path &candidate);
bool normalize_project_path(std::string_view input, std::string &out);
bool resolve_project_include(std::string_view importer, std::string_view include,
                             std::string &out);
std::string trim_bom(std::string bytes);
std::pair<uint64_t, uint64_t> file_fingerprint(std::string_view bytes);

struct ScriptVm {
    ScriptCard &card;
    Backend backend;
    std::string main_relative;
    std::string current_source;
    size_t loaded_source_bytes = 0;
    uint64_t next_callback_id = 1;
    bool active = false;
    bool limit_exceeded = false;
    size_t callback_depth = 0;
    std::set<std::string> loaded_paths;
    std::unordered_map<std::string, std::string> source_cache;
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> file_snapshots;
    std::vector<TimerEntry> timers;
    std::vector<SignalEntry> subscriptions;

    ScriptVm(ScriptCard &c, Backend b, std::string path)
        : card(c), backend(b), main_relative(std::move(path)) {}
    virtual ~ScriptVm();
    virtual bool initialize(std::string &error) = 0;
    virtual bool call_reset(bool cold, std::string &error) = 0;
    virtual bool call_read(uint64_t address, uint8_t &value, std::string &error) = 0;
    virtual bool call_write(uint64_t address, uint8_t value, std::string &error) = 0;
    virtual bool invoke_callback(uint64_t id, std::string_view kind,
                                 std::string_view name, int64_t value,
                                 std::string &error) = 0;
    virtual bool save_user_state(std::string &state, std::string &error) = 0;
    virtual bool load_user_state(std::string_view state, std::string &error) = 0;
    // Lua uses the stack index and JavaScript the JSValue. Backends retaining
    // callbacks outside this translation unit can pass their opaque IDs to
    // register_retained_timer/signal instead; all share the host registration.
    virtual uint64_t retain_callback(int function_index, JSValueConst js_function) = 0;
    virtual void release_function(uint64_t id) = 0;

    bool source(std::string_view relative, std::string &text, std::string &error) {
        const std::string name(relative);
        if (auto found = source_cache.find(name); found != source_cache.end()) {
            text = found->second;
            return true;
        }
        std::vector<uint8_t> bytes;
        const SrhStatus status = card.read_project(relative, bytes);
        if (status != SRH_OK) {
            error = "Cannot load " + name + ": " + status_text(status);
            return false;
        }
        if (bytes.size() > kSourceLimit - loaded_source_bytes) {
            error = "Script source limit exceeded (16 MiB total)";
            return false;
        }
        loaded_source_bytes += bytes.size();
        const char *data = bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data());
        text.assign(data, bytes.size());
        text = trim_bom(std::move(text));
        loaded_paths.insert(name);
        source_cache.emplace(name, text);
        return true;
    }
    static const char *status_text(SrhStatus status) {
        switch (status) {
        case SRH_NOT_FOUND: return "file not found";
        case SRH_UNAVAILABLE: return "no active project is available";
        case SRH_INVALID:
            return "invalid project path, protected project.json write, or file exceeds 16 MiB";
        case SRH_ERROR: return "project file access failed";
        default: return "project file access is unavailable";
        }
    }
    uint64_t register_timer(uint64_t delay, int function_index,
                            JSValueConst js_function = JS_UNDEFINED);
    uint64_t register_retained_timer(uint64_t delay, uint64_t id);
    bool register_signal(std::string_view name, int function_index, std::string &error,
                         JSValueConst js_function = JS_UNDEFINED);
    bool register_retained_signal(std::string_view name, uint64_t id, std::string &error);
    bool activate(std::string &error);
    void deactivate();
    SrhStatus timer_fired(TimerContext *context);
    void log_error(std::string_view where, std::string_view message) {
        card.log("script [" + std::string(where) + "]: " + std::string(message));
    }
};

struct LuaVm final : ScriptVm {
    lua_State *state = nullptr;
    size_t allocated = 0;
    uint64_t hook_ticks = 0;
    std::vector<std::string> import_stack;
    std::map<uint64_t, int> lua_callbacks;

    LuaVm(ScriptCard &card, std::string path) : ScriptVm(card, Backend::lua, std::move(path)) {}
    ~LuaVm() override;
    bool initialize(std::string &error) override;
    bool call_reset(bool cold, std::string &error) override;
    bool call_read(uint64_t address, uint8_t &value, std::string &error) override;
    bool call_write(uint64_t address, uint8_t value, std::string &error) override;
    bool invoke_callback(uint64_t id, std::string_view kind, std::string_view name,
                         int64_t value, std::string &error) override;
    bool save_user_state(std::string &state, std::string &error) override;
    bool load_user_state(std::string_view state, std::string &error) override;
    uint64_t retain_callback(int function_index, JSValueConst js_function) override;
    void release_function(uint64_t id) override;
    bool load_chunk(std::string_view relative, std::string_view source, int results,
                    std::string &error);
    static void *memory_allocator(void *opaque, void *pointer, size_t old_size,
                                  size_t new_size);
    static void instruction_hook(lua_State *state, lua_Debug *debug);

    static LuaVm *from(lua_State *state) {
        return static_cast<LuaVm *>(lua_touserdata(state, lua_upvalueindex(1)));
    }
    static void push_api(lua_State *state, LuaVm *vm, lua_CFunction function,
                         const char *name);
    static int api_log(lua_State *state);
    static int api_bus_read(lua_State *state);
    static int api_bus_write(lua_State *state);
    static int api_time(lua_State *state);
    static int api_signal_read(lua_State *state);
    static int api_signal_drive(lua_State *state);
    static int api_on_signal(lua_State *state);
    static int api_after(lua_State *state);
    static int api_project_read(lua_State *state);
    static int api_project_write(lua_State *state);
    static int api_require(lua_State *state);
    static int api_print(lua_State *state);
};

struct JsVm final : ScriptVm {
    JSRuntime *runtime = nullptr;
    JSContext *context = nullptr;
    JSValue main_exports = JS_UNDEFINED;
    JSModuleDef *main_module = nullptr;
    uint64_t interrupt_ticks = 0;
    std::string active_source;
    std::map<uint64_t, JSValue> js_callbacks;

    JsVm(ScriptCard &card, std::string path) : ScriptVm(card, Backend::javascript, std::move(path)) {}
    ~JsVm() override;
    bool initialize(std::string &error) override;
    bool call_reset(bool cold, std::string &error) override;
    bool call_read(uint64_t address, uint8_t &value, std::string &error) override;
    bool call_write(uint64_t address, uint8_t value, std::string &error) override;
    bool invoke_callback(uint64_t id, std::string_view kind, std::string_view name,
                         int64_t value, std::string &error) override;
    bool save_user_state(std::string &state, std::string &error) override;
    bool load_user_state(std::string_view state, std::string &error) override;
    uint64_t retain_callback(int function_index, JSValueConst js_function) override;
    void release_function(uint64_t id) override;
    bool evaluate_module(std::string_view relative, std::string_view source,
                         std::string &error);
    bool run_jobs(std::string &error);
    JSValue get_hook(const char *name);
    std::string exception_text();
    std::string promise_error_text(JSValueConst promise);
    static int interrupt(JSRuntime *runtime, void *opaque);
    static char *normalize_module(JSContext *context, const char *base,
                                  const char *name, void *opaque);
    static JSModuleDef *load_module(JSContext *context, const char *name, void *opaque);
    static JsVm *from(JSContext *context) {
        return static_cast<JsVm *>(JS_GetContextOpaque(context));
    }
    static JSValue api_log(JSContext *, JSValueConst, int, JSValueConst *);
    static JSValue api_bus_read(JSContext *, JSValueConst, int, JSValueConst *);
    static JSValue api_bus_write(JSContext *, JSValueConst, int, JSValueConst *);
    static JSValue api_time(JSContext *, JSValueConst, int, JSValueConst *);
    static JSValue api_signal_read(JSContext *, JSValueConst, int, JSValueConst *);
    static JSValue api_signal_drive(JSContext *, JSValueConst, int, JSValueConst *);
    static JSValue api_on_signal(JSContext *, JSValueConst, int, JSValueConst *);
    static JSValue api_after(JSContext *, JSValueConst, int, JSValueConst *);
    static JSValue api_project_read(JSContext *, JSValueConst, int, JSValueConst *);
    static JSValue api_project_write(JSContext *, JSValueConst, int, JSValueConst *);
};

struct MrubyVm final : ScriptVm {
    mrb_state *state = nullptr;
    uint64_t instructions = 0;
    std::map<uint64_t, mrb_value> callbacks;
    std::set<std::string> required_paths;
    std::vector<std::string> import_stack;
    MrubyVm(ScriptCard &card, std::string path)
        : ScriptVm(card, Backend::mruby, std::move(path)) {}
    ~MrubyVm() override;
    bool initialize(std::string &error) override;
    bool call_reset(bool cold, std::string &error) override;
    bool call_read(uint64_t address, uint8_t &value, std::string &error) override;
    bool call_write(uint64_t address, uint8_t value, std::string &error) override;
    bool invoke_callback(uint64_t id, std::string_view kind, std::string_view name,
                         int64_t value, std::string &error) override;
    bool save_user_state(std::string &state, std::string &error) override;
    bool load_user_state(std::string_view state, std::string &error) override;
    // Python retains callbacks through retain() and the shared registry; the
    // Lua stack index / JavaScript value entry point stays unused.
    uint64_t retain_callback(int, JSValueConst) override { return 0; }
    void release_function(uint64_t id) override;
    uint64_t retain(mrb_value callback);
    bool call_hook(const char *name, mrb_int argc, mrb_value *argv,
                   mrb_value &result, std::string &error);
    std::string exception_text();
    static void instruction_hook(mrb_state *mrb, const mrb_irep *, const mrb_code *, mrb_value *);
    static MrubyVm *from(mrb_state *mrb) { return static_cast<MrubyVm *>(mrb->ud); }
};

struct PythonVm final : ScriptVm {
    int slot = -1;
    uint64_t instructions_remaining = 0;
    PythonVm(ScriptCard &card, std::string path)
        : ScriptVm(card, Backend::python, std::move(path)) {}
    ~PythonVm() override;
    bool initialize(std::string &error) override;
    bool call_reset(bool cold, std::string &error) override;
    bool call_read(uint64_t address, uint8_t &value, std::string &error) override;
    bool call_write(uint64_t address, uint8_t value, std::string &error) override;
    bool invoke_callback(uint64_t id, std::string_view kind, std::string_view name,
                         int64_t value, std::string &error) override;
    bool save_user_state(std::string &state, std::string &error) override;
    bool load_user_state(std::string_view state, std::string &error) override;
    uint64_t retain_callback(int, JSValueConst) override { return 0; }
    void release_function(uint64_t id) override;
    uint64_t retain(py_Ref callback);
    bool call(py_Ref function, const std::vector<int64_t> &args, std::string &error);
    bool call_named(const char *name, const std::vector<int64_t> &args,
                    std::string &error);
};

#ifdef SRZ80_SCRIPT_PHP
std::unique_ptr<ScriptVm> make_php_vm(ScriptCard &card, std::string path);
#endif

struct CallbackScope {
    size_t &depth;
    bool entered;
    explicit CallbackScope(size_t &value) : depth(value), entered(value < kCallbackDepthLimit) {
        if (entered)
            ++depth;
    }
    ~CallbackScope() {
        if (entered)
            --depth;
    }
};

} // namespace srz80_script
