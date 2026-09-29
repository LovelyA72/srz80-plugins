#include "script_card.h"

namespace srz80_script {

namespace {

void push_lua_api(lua_State *state, LuaVm *vm, lua_CFunction function) {
    lua_pushlightuserdata(state, vm);
    lua_pushcclosure(state, function, 1);
}

void lua_push_api_table(lua_State *state, LuaVm *vm, bool project) {
    lua_newtable(state);
    if (project) {
        push_lua_api(state, vm, LuaVm::api_project_read);
        lua_setfield(state, -2, "read");
        push_lua_api(state, vm, LuaVm::api_project_write);
        lua_setfield(state, -2, "write");
        lua_setglobal(state, "project");
        return;
    }
    push_lua_api(state, vm, LuaVm::api_log);
    lua_setfield(state, -2, "log");
    push_lua_api(state, vm, LuaVm::api_bus_read);
    lua_setfield(state, -2, "read");
    push_lua_api(state, vm, LuaVm::api_bus_write);
    lua_setfield(state, -2, "write");
    push_lua_api(state, vm, LuaVm::api_time);
    lua_setfield(state, -2, "time_ns");
    push_lua_api(state, vm, LuaVm::api_signal_read);
    lua_setfield(state, -2, "signal_read");
    push_lua_api(state, vm, LuaVm::api_signal_drive);
    lua_setfield(state, -2, "signal_drive");
    push_lua_api(state, vm, LuaVm::api_on_signal);
    lua_setfield(state, -2, "on_signal");
    push_lua_api(state, vm, LuaVm::api_after);
    lua_setfield(state, -2, "after");
    lua_setglobal(state, "card");
}

int lua_api_failure(lua_State *state, const char *message) {
    lua_pushnil(state);
    lua_pushstring(state, message);
    return 2;
}

int lua_raise_api_failure(lua_State *state) {
    lua_pushliteral(state, "script host API failed");
    return lua_error(state);
}

bool lua_value_to_json(lua_State *state, int index, Json &out, unsigned depth,
                       std::set<const void *> &active, std::string &error) {
    if (depth > 64) {
        error = "state nesting exceeds 64 levels";
        return false;
    }
    index = lua_absindex(state, index);
    switch (lua_type(state, index)) {
    case LUA_TBOOLEAN:
        out = lua_toboolean(state, index) != 0;
        return true;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index)) {
            int valid = 0;
            const lua_Integer value = lua_tointegerx(state, index, &valid);
            if (!valid) {
                error = "invalid integer in state";
                return false;
            }
            out = static_cast<int64_t>(value);
        } else {
            const lua_Number value = lua_tonumber(state, index);
            if (!std::isfinite(value)) {
                error = "state numbers must be finite";
                return false;
            }
            out = static_cast<double>(value);
        }
        return true;
    case LUA_TSTRING: {
        size_t size = 0;
        const char *text = lua_tolstring(state, index, &size);
        try {
            out = std::string(text, size);
            (void)out.dump();
        } catch (...) {
            error = "state strings must be valid UTF-8";
            return false;
        }
        return true;
    }
    case LUA_TTABLE: {
        const void *identity = lua_topointer(state, index);
        if (!active.insert(identity).second) {
            error = "state tables cannot contain cycles";
            return false;
        }
        std::map<lua_Integer, Json> array_items;
        std::map<std::string, Json> object_items;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            Json value;
            if (!lua_value_to_json(state, -1, value, depth + 1, active, error)) {
                lua_pop(state, 2);
                active.erase(identity);
                return false;
            }
            if (lua_type(state, -2) == LUA_TNUMBER && lua_isinteger(state, -2)) {
                int valid = 0;
                const lua_Integer key = lua_tointegerx(state, -2, &valid);
                if (!valid || key < 1 || object_items.size()) {
                    lua_pop(state, 2);
                    active.erase(identity);
                    error = "state tables must use either dense array or string keys";
                    return false;
                }
                array_items.emplace(key, std::move(value));
            } else if (lua_type(state, -2) == LUA_TSTRING && array_items.empty()) {
                size_t length = 0;
                const char *key = lua_tolstring(state, -2, &length);
                object_items.emplace(std::string(key, length), std::move(value));
            } else {
                lua_pop(state, 2);
                active.erase(identity);
                error = "state tables must use either dense array or string keys";
                return false;
            }
            lua_pop(state, 1);
        }
        active.erase(identity);
        if (array_items.empty() && object_items.empty()) {
            out = Json::object();
            return true;
        }
        if (!array_items.empty()) {
            if (array_items.size() != static_cast<size_t>(array_items.rbegin()->first)) {
                error = "state arrays must not contain holes";
                return false;
            }
            out = Json::array();
            for (const auto &[key, value] : array_items) {
                (void)key;
                out.push_back(value);
            }
        } else {
            out = Json::object();
            for (auto &[key, value] : object_items)
                out.emplace(std::move(key), std::move(value));
        }
        return true;
    }
    default:
        error = "state values must be booleans, numbers, strings, or plain tables";
        return false;
    }
}

bool lua_push_json(lua_State *state, const Json &value, unsigned depth) {
    if (depth > 64 || value.is_null())
        return false;
    if (value.is_boolean()) {
        lua_pushboolean(state, value.get<bool>());
    } else if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        if (number < std::numeric_limits<lua_Integer>::min() ||
            number > std::numeric_limits<lua_Integer>::max())
            return false;
        lua_pushinteger(state, static_cast<lua_Integer>(number));
    } else if (value.is_number()) {
        const double number = value.get<double>();
        if (!std::isfinite(number))
            return false;
        lua_pushnumber(state, number);
    } else if (value.is_string()) {
        const auto &text = value.get_ref<const std::string &>();
        lua_pushlstring(state, text.data(), text.size());
    } else if (value.is_array()) {
        lua_createtable(state, static_cast<int>(value.size()), 0);
        lua_Integer index = 1;
        for (const auto &item : value) {
            if (!lua_push_json(state, item, depth + 1))
                return false;
            lua_rawseti(state, -2, index++);
        }
    } else if (value.is_object()) {
        lua_createtable(state, 0, static_cast<int>(value.size()));
        for (auto it = value.begin(); it != value.end(); ++it) {
            lua_pushlstring(state, it.key().data(), it.key().size());
            if (!lua_push_json(state, it.value(), depth + 1))
                return false;
            lua_rawset(state, -3);
        }
    } else {
        return false;
    }
    return true;
}

} // namespace

LuaVm::~LuaVm() {
    if (state)
        lua_close(state);
}

void *LuaVm::memory_allocator(void *opaque, void *pointer, size_t old_size,
                              size_t new_size) {
    auto *vm = static_cast<LuaVm *>(opaque);
    const size_t old_amount = pointer ? old_size : 0;
    if (!new_size) {
        std::free(pointer);
        vm->allocated = old_amount <= vm->allocated ? vm->allocated - old_amount : 0;
        return nullptr;
    }
    if (new_size > old_amount && new_size - old_amount > kVmMemoryLimit -
        std::min(vm->allocated, kVmMemoryLimit))
        return nullptr;
    void *result = std::realloc(pointer, new_size);
    if (result) {
        vm->allocated = vm->allocated - std::min(vm->allocated, old_amount) + new_size;
    }
    return result;
}

bool LuaVm::initialize(std::string &error) {
    state = lua_newstate(memory_allocator, this, 0x53525a38u);
    if (!state) {
        error = "Cannot create Lua 5.5 VM (64 MiB memory limit)";
        return false;
    }
    luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(state, 1);
    // These base functions open a native file or compile an unmetered chunk.
    for (const char *name : {"dofile", "loadfile", "load"}) {
        lua_pushnil(state);
        lua_setglobal(state, name);
    }
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, api_require, 1);
    lua_setglobal(state, "require");
    lua_pushlightuserdata(state, this);
    lua_pushcclosure(state, api_print, 1);
    lua_setglobal(state, "print");
    lua_pushlightuserdata(state, this);
    lua_setglobal(state, "__srz80_vm");
    lua_newtable(state);
    lua_setglobal(state, "__srz80_modules");
    lua_newtable(state);
    lua_setglobal(state, "state");
    lua_push_api_table(state, this, false);
    lua_push_api_table(state, this, true);

    const auto main = source_cache.find(main_relative);
    if (main == source_cache.end()) {
        error = "Main script source was not loaded";
        return false;
    }
    current_source = main_relative;
    import_stack.push_back(main_relative);
    const bool ok = load_chunk(main_relative, main->second, 0, error);
    import_stack.pop_back();
    if (!ok) {
        log_error(main_relative, error);
        return false;
    }
    return true;
}

bool LuaVm::load_chunk(std::string_view relative, std::string_view source_text, int results,
                       std::string &error) {
    const int top = lua_gettop(state);
    const lua_Hook previous_hook = lua_gethook(state);
    const int previous_mask = lua_gethookmask(state);
    const int previous_count = lua_gethookcount(state);
    current_source.assign(relative);
    if (!previous_hook) {
        hook_ticks = 0;
        limit_exceeded = false;
        lua_sethook(state, instruction_hook, LUA_MASKCOUNT, kLuaHookStep);
    }
    const std::string chunk_name = "@" + std::string(relative);
    int status = luaL_loadbufferx(state, source_text.data(), source_text.size(),
                                  chunk_name.c_str(), "t");
    if (status == LUA_OK)
        status = lua_pcall(state, 0, results, 0);
    lua_sethook(state, previous_hook, previous_mask, previous_count);
    if (status != LUA_OK) {
        size_t size = 0;
        const char *message = lua_tolstring(state, -1, &size);
        error.assign(message ? message : "Lua script error", message ? size : 16);
        lua_settop(state, top);
        return false;
    }
    if (results == 0)
        lua_settop(state, top);
    return true;
}

void LuaVm::instruction_hook(lua_State *state, lua_Debug *) {
    void *opaque = nullptr;
    lua_getallocf(state, &opaque);
    auto *vm = static_cast<LuaVm *>(opaque);
    if (!vm)
        return;
    vm->hook_ticks += kLuaHookStep;
    if (vm->hook_ticks > kLuaInstructionLimit) {
        vm->limit_exceeded = true;
        lua_pushliteral(state, "callback instruction limit exceeded (10 million)");
        lua_error(state);
    }
}

uint64_t LuaVm::retain_callback(int function_index, JSValueConst js_function) {
    (void)js_function;
    if (!lua_isfunction(state, function_index))
        return 0;
    const uint64_t id = next_callback_id++;
    lua_pushvalue(state, function_index);
    lua_callbacks.emplace(id, luaL_ref(state, LUA_REGISTRYINDEX));
    return id;
}

void LuaVm::release_function(uint64_t id) {
    const auto found = lua_callbacks.find(id);
    if (found == lua_callbacks.end())
        return;
    luaL_unref(state, LUA_REGISTRYINDEX, found->second);
    lua_callbacks.erase(found);
}

bool LuaVm::call_reset(bool cold, std::string &error) {
    CallbackScope callback(callback_depth);
    if (!callback.entered) {
        error = "script callback nesting limit exceeded (64)";
        return false;
    }
    current_source = main_relative;
    const int top = lua_gettop(state);
    lua_getglobal(state, "on_reset");
    if (lua_isnil(state, -1)) {
        lua_settop(state, top);
        return true;
    }
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, top);
        error = "on_reset must be a function";
        return false;
    }
    lua_pushboolean(state, cold);
    hook_ticks = 0;
    limit_exceeded = false;
    lua_sethook(state, instruction_hook, LUA_MASKCOUNT, kLuaHookStep);
    const int status = lua_pcall(state, 1, 0, 0);
    lua_sethook(state, nullptr, 0, 0);
    if (status != LUA_OK) {
        size_t size = 0;
        const char *message = lua_tolstring(state, -1, &size);
        error.assign(message ? message : "Lua reset callback failed",
                     message ? size : 27);
        lua_settop(state, top);
        return false;
    }
    lua_settop(state, top);
    return true;
}

bool LuaVm::call_read(uint64_t address, uint8_t &value, std::string &error) {
    CallbackScope callback(callback_depth);
    if (!callback.entered) {
        error = "script callback nesting limit exceeded (64)";
        return false;
    }
    current_source = main_relative;
    const int top = lua_gettop(state);
    lua_getglobal(state, "on_read");
    if (lua_isnil(state, -1)) {
        lua_settop(state, top);
        value = 0;
        return true;
    }
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, top);
        error = "on_read must be a function";
        return false;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(address));
    hook_ticks = 0;
    limit_exceeded = false;
    lua_sethook(state, instruction_hook, LUA_MASKCOUNT, kLuaHookStep);
    const int status = lua_pcall(state, 1, 1, 0);
    lua_sethook(state, nullptr, 0, 0);
    if (status != LUA_OK) {
        size_t size = 0;
        const char *message = lua_tolstring(state, -1, &size);
        error.assign(message ? message : "Lua read callback failed",
                     message ? size : 25);
        lua_settop(state, top);
        return false;
    }
    if (lua_isnil(state, -1)) {
        value = 0;
    } else if (lua_isinteger(state, -1)) {
        int valid = 0;
        const lua_Integer result = lua_tointegerx(state, -1, &valid);
        if (!valid || result < 0 || result > 255) {
            error = "on_read must return a byte from 0 to 255";
            lua_settop(state, top);
            return false;
        }
        value = static_cast<uint8_t>(result);
    } else {
        error = "on_read must return an integer byte";
        lua_settop(state, top);
        return false;
    }
    lua_settop(state, top);
    return true;
}

bool LuaVm::call_write(uint64_t address, uint8_t value, std::string &error) {
    CallbackScope callback(callback_depth);
    if (!callback.entered) {
        error = "script callback nesting limit exceeded (64)";
        return false;
    }
    current_source = main_relative;
    const int top = lua_gettop(state);
    lua_getglobal(state, "on_write");
    if (lua_isnil(state, -1)) {
        lua_settop(state, top);
        return true;
    }
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, top);
        error = "on_write must be a function";
        return false;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(address));
    lua_pushinteger(state, value);
    hook_ticks = 0;
    limit_exceeded = false;
    lua_sethook(state, instruction_hook, LUA_MASKCOUNT, kLuaHookStep);
    const int status = lua_pcall(state, 2, 0, 0);
    lua_sethook(state, nullptr, 0, 0);
    if (status != LUA_OK) {
        size_t size = 0;
        const char *message = lua_tolstring(state, -1, &size);
        error.assign(message ? message : "Lua write callback failed",
                     message ? size : 26);
        lua_settop(state, top);
        return false;
    }
    lua_settop(state, top);
    return true;
}

bool LuaVm::invoke_callback(uint64_t id, std::string_view kind, std::string_view name,
                            int64_t value, std::string &error) {
    CallbackScope callback(callback_depth);
    if (!callback.entered) {
        error = "script callback nesting limit exceeded (64)";
        return false;
    }
    current_source = main_relative;
    const auto found = lua_callbacks.find(id);
    if (found == lua_callbacks.end())
        return true;
    const int top = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, found->second);
    int nargs = 0;
    if (kind == "signal") {
        lua_pushlstring(state, name.data(), name.size());
        lua_pushinteger(state, static_cast<lua_Integer>(value));
        nargs = 2;
    } else {
        lua_pushinteger(state, static_cast<lua_Integer>(value));
        nargs = 1;
    }
    hook_ticks = 0;
    limit_exceeded = false;
    lua_sethook(state, instruction_hook, LUA_MASKCOUNT, kLuaHookStep);
    const int status = lua_pcall(state, nargs, 0, 0);
    lua_sethook(state, nullptr, 0, 0);
    if (status != LUA_OK) {
        size_t size = 0;
        const char *message = lua_tolstring(state, -1, &size);
        error.assign(message ? message : "Lua callback failed", message ? size : 19);
        lua_settop(state, top);
        return false;
    }
    lua_settop(state, top);
    return true;
}

bool LuaVm::save_user_state(std::string &saved, std::string &error) {
    const int top = lua_gettop(state);
    lua_getglobal(state, "state");
    Json value;
    std::set<const void *> active_tables;
    if (!lua_istable(state, -1) ||
        !lua_value_to_json(state, -1, value, 0, active_tables, error) ||
        !value.is_object()) {
        if (error.empty())
            error = "global state must be a JSON object";
        lua_settop(state, top);
        return false;
    }
    try {
        saved = value.dump();
    } catch (...) {
        error = "state could not be encoded as JSON";
        lua_settop(state, top);
        return false;
    }
    lua_settop(state, top);
    if (saved.size() > kStateLimit) {
        error = "saved state exceeds 1 MiB";
        return false;
    }
    return true;
}

bool LuaVm::load_user_state(std::string_view saved, std::string &error) {
    Json value;
    try {
        value = Json::parse(saved.begin(), saved.end());
    } catch (...) {
        error = "saved state is malformed JSON";
        return false;
    }
    if (!value.is_object() || value.dump().size() > kStateLimit) {
        error = "saved state must be a JSON object of at most 1 MiB";
        return false;
    }
    const int top = lua_gettop(state);
    if (!lua_push_json(state, value, 0)) {
        lua_settop(state, top);
        error = "saved state contains a value Lua cannot represent";
        return false;
    }
    lua_setglobal(state, "state");
    return true;
}

int LuaVm::api_log(lua_State *state) {
    auto *vm = from(state);
    try {
        size_t size = 0;
        const char *text = luaL_tolstring(state, 1, &size);
        const std::string message(text ? text : "", size);
        lua_pop(state, 1);
        vm->card.log("script [" + vm->current_source + "]: " + message);
        return 0;
    } catch (...) {
        return lua_raise_api_failure(state);
    }
}

int LuaVm::api_bus_read(lua_State *state) {
    auto *vm = from(state);
    try {
        const std::string space_name = luaL_checkstring(state, 1);
        const lua_Integer address = luaL_checkinteger(state, 2);
        if (address < 0)
            return lua_api_failure(state, "address must be non-negative");
        SrhHandle space = 0;
        uint8_t value = 0;
        if (!vm->card.lookup_space(space_name, space))
            return lua_api_failure(state, "address space not found");
        const SrhStatus status = vm->card.host->read
            ? vm->card.host->read(vm->card.host->context, vm->card.owner, space,
                                  static_cast<uint64_t>(address), &value)
            : SRH_UNAVAILABLE;
        if (status != SRH_OK)
            return lua_api_failure(state, "bus read failed");
        lua_pushinteger(state, value);
        return 1;
    } catch (...) {
        return lua_raise_api_failure(state);
    }
}

int LuaVm::api_bus_write(lua_State *state) {
    auto *vm = from(state);
    try {
        const std::string space_name = luaL_checkstring(state, 1);
        const lua_Integer address = luaL_checkinteger(state, 2);
        const lua_Integer byte = luaL_checkinteger(state, 3);
        if (address < 0 || byte < 0 || byte > 255)
            return lua_api_failure(state, "address or byte is out of range");
        SrhHandle space = 0;
        if (!vm->card.lookup_space(space_name, space))
            return lua_api_failure(state, "address space not found");
        const SrhStatus status = vm->card.host->write
            ? vm->card.host->write(vm->card.host->context, vm->card.owner, space,
                                   static_cast<uint64_t>(address),
                                   static_cast<uint8_t>(byte))
            : SRH_UNAVAILABLE;
        if (status != SRH_OK)
            return lua_api_failure(state, "bus write failed");
        lua_pushboolean(state, 1);
        return 1;
    } catch (...) {
        return lua_raise_api_failure(state);
    }
}

int LuaVm::api_time(lua_State *state) {
    auto *vm = from(state);
    uint64_t now = 0;
    if (!vm->card.host->time_ns ||
        vm->card.host->time_ns(vm->card.host->context, &now) != SRH_OK)
        return lua_api_failure(state, "simulation time is unavailable");
    lua_pushnumber(state, static_cast<lua_Number>(now));
    return 1;
}

int LuaVm::api_signal_read(lua_State *state) {
    auto *vm = from(state);
    try {
        const std::string name = luaL_checkstring(state, 1);
        SrhHandle signal = 0;
        int32_t millivolts = 0;
        if (!vm->card.lookup_signal(name, signal) || !vm->card.host->signal_read ||
            vm->card.host->signal_read(vm->card.host->context, signal, &millivolts) != SRH_OK)
            return lua_api_failure(state, "signal read failed");
        lua_pushinteger(state, millivolts);
        return 1;
    } catch (...) {
        return lua_raise_api_failure(state);
    }
}

int LuaVm::api_signal_drive(lua_State *state) {
    auto *vm = from(state);
    try {
        const std::string name = luaL_checkstring(state, 1);
        const lua_Integer millivolts = luaL_checkinteger(state, 2);
        const lua_Integer strength = luaL_optinteger(state, 3, 0);
        if (millivolts < INT32_MIN || millivolts > INT32_MAX ||
            strength < INT32_MIN || strength > INT32_MAX)
            return lua_api_failure(state, "signal level or strength is out of range");
        SrhHandle signal = 0;
        if (!vm->card.lookup_signal(name, signal) || !vm->card.host->signal_drive ||
            vm->card.host->signal_drive(vm->card.host->context, vm->card.owner, signal,
                                        static_cast<int32_t>(millivolts),
                                        static_cast<int32_t>(strength)) != SRH_OK)
            return lua_api_failure(state, "signal drive failed");
        lua_pushboolean(state, 1);
        return 1;
    } catch (...) {
        return lua_raise_api_failure(state);
    }
}

int LuaVm::api_on_signal(lua_State *state) {
    auto *vm = from(state);
    try {
        const std::string name = luaL_checkstring(state, 1);
        std::string error;
        if (!vm->register_signal(name, 2, error))
            return lua_api_failure(state, error.c_str());
        lua_pushboolean(state, 1);
        return 1;
    } catch (...) {
        return lua_raise_api_failure(state);
    }
}

int LuaVm::api_after(lua_State *state) {
    auto *vm = from(state);
    try {
        const lua_Integer delay = luaL_checkinteger(state, 1);
        if (delay < 0)
            return lua_api_failure(state, "timer delay must be non-negative");
        const uint64_t id = vm->register_timer(static_cast<uint64_t>(delay), 2);
        if (!id)
            return lua_api_failure(state, "cannot schedule timer");
        lua_pushinteger(state, static_cast<lua_Integer>(id));
        return 1;
    } catch (...) {
        return lua_raise_api_failure(state);
    }
}

int LuaVm::api_project_read(lua_State *state) {
    auto *vm = from(state);
    try {
        const std::string path = luaL_checkstring(state, 1);
        std::vector<uint8_t> bytes;
        const SrhStatus status = vm->card.read_project(path, bytes);
        if (status != SRH_OK)
            return lua_api_failure(state, ScriptVm::status_text(status));
        const char *data = bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data());
        std::string normalized;
        if (normalize_project_path(path, normalized))
            vm->file_snapshots[normalized] = file_fingerprint({data, bytes.size()});
        lua_pushlstring(state, data, bytes.size());
        return 1;
    } catch (...) {
        return lua_raise_api_failure(state);
    }
}

int LuaVm::api_project_write(lua_State *state) {
    auto *vm = from(state);
    try {
        const std::string path = luaL_checkstring(state, 1);
        size_t size = 0;
        const char *data = luaL_checklstring(state, 2, &size);
        const SrhStatus status = vm->card.write_project(
            path, reinterpret_cast<const uint8_t *>(data), size);
        if (status != SRH_OK)
            return lua_api_failure(state, ScriptVm::status_text(status));
        lua_pushboolean(state, 1);
        return 1;
    } catch (...) {
        return lua_raise_api_failure(state);
    }
}

int LuaVm::api_require(lua_State *state) {
    auto *vm = from(state);
    try {
        size_t size = 0;
        const char *name = luaL_checklstring(state, 1, &size);
        if (!name || !size)
            return lua_api_failure(state, "require path must not be empty");
        std::string requested(name, size);
        if (requested.find('\0') != std::string::npos)
            return lua_api_failure(state, "require path contains NUL");
        if (requested.find('.') == std::string::npos &&
            requested.find('/') == std::string::npos &&
            requested.find('\\') == std::string::npos)
            requested += ".lua";
        else if (fs::path(requested).extension().empty())
            requested += ".lua";
        if (fs::path(requested).extension() != ".lua")
            return lua_api_failure(state, "Lua require accepts only .lua modules");
        std::string relative;
        const std::string_view importer = vm->import_stack.empty()
            ? std::string_view(vm->main_relative)
            : std::string_view(vm->import_stack.back());
        if (!resolve_project_include(importer, requested, relative))
            return lua_api_failure(state, "require path escapes the project or is invalid");

        lua_getglobal(state, "__srz80_modules");
        lua_getfield(state, -1, relative.c_str());
        if (!lua_isnil(state, -1)) {
            lua_remove(state, -2);
            return 1;
        }
        lua_pop(state, 1);
        // The in-progress export table is visible to cyclic requires.
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_setfield(state, -3, relative.c_str());
        lua_pop(state, 2);
        std::string error;
        std::string source;
        if (!vm->source(relative, source, error)) {
            vm->log_error(relative, error);
            lua_getglobal(state, "__srz80_modules");
            lua_pushnil(state);
            lua_setfield(state, -2, relative.c_str());
            lua_pop(state, 1);
            lua_pushnil(state);
            lua_pushstring(state, error.c_str());
            return 2;
        }
        vm->import_stack.push_back(relative);
        const bool loaded = vm->load_chunk(relative, source, 1, error);
        vm->import_stack.pop_back();
        if (!loaded) {
            vm->log_error(relative, error);
            lua_getglobal(state, "__srz80_modules");
            lua_pushnil(state);
            lua_setfield(state, -2, relative.c_str());
            lua_pop(state, 1);
            lua_pushnil(state);
            lua_pushstring(state, error.c_str());
            return 2;
        }
        const bool returned_value = !lua_isnil(state, -1);
        if (returned_value) {
            lua_getglobal(state, "__srz80_modules");
            lua_pushvalue(state, -2);
            lua_setfield(state, -2, relative.c_str());
            lua_pop(state, 1);
        } else {
            lua_pop(state, 1);
            lua_getglobal(state, "__srz80_modules");
            lua_getfield(state, -1, relative.c_str());
            lua_remove(state, -2);
        }
        return 1;
    } catch (...) {
        return lua_raise_api_failure(state);
    }
}

int LuaVm::api_print(lua_State *state) {
    auto *vm = from(state);
    try {
        std::ostringstream output;
        const int count = lua_gettop(state);
        for (int index = 1; index <= count; ++index) {
            if (index > 1)
                output << '\t';
            size_t size = 0;
            const char *text = luaL_tolstring(state, index, &size);
            output.write(text ? text : "", static_cast<std::streamsize>(size));
            lua_pop(state, 1);
        }
        vm->card.log("script [" + vm->current_source + "]: " + output.str());
        return 0;
    } catch (...) {
        return lua_raise_api_failure(state);
    }
}

} // namespace srz80_script
