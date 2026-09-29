#include "script_card.h"

namespace srz80_script {

namespace {

JSValue js_error(JSContext *context, const std::string &message) {
    return JS_ThrowTypeError(context, "%s", message.c_str());
}

bool js_string(JSContext *context, JSValueConst value, std::string &out,
               std::string &error) {
    size_t size = 0;
    const char *text = JS_ToCStringLen2(context, &size, value, false);
    if (!text) {
        error = "expected a string";
        return false;
    }
    out.assign(text, size);
    JS_FreeCString(context, text);
    if (out.find('\0') != std::string::npos) {
        error = "string must not contain NUL";
        return false;
    }
    return true;
}

bool js_integer(JSContext *context, JSValueConst value, int64_t &out) {
    if (!JS_IsNumber(value))
        return false;
    double number = 0;
    if (JS_ToFloat64(context, &number, value) < 0 || !std::isfinite(number) ||
        std::floor(number) != number ||
        number < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
        number >= 9223372036854775808.0)
        return false;
    out = static_cast<int64_t>(number);
    return true;
}

void js_set_api(JSContext *context, JSValue object, const char *name,
                JSCFunction *function, int argument_count) {
    JS_SetPropertyStr(context, object, name,
                      JS_NewCFunction(context, function, name, argument_count));
}

} // namespace

JsVm::~JsVm() {
    deactivate();
    for (const auto &[id, value] : js_callbacks) {
        (void)id;
        if (context)
            JS_FreeValue(context, value);
    }
    js_callbacks.clear();
    if (context && !JS_IsUndefined(main_exports))
        JS_FreeValue(context, main_exports);
    if (context)
        JS_FreeContext(context);
    if (runtime)
        JS_FreeRuntime(runtime);
}

int JsVm::interrupt(JSRuntime *, void *opaque) {
    auto *vm = static_cast<JsVm *>(opaque);
    if (++vm->interrupt_ticks >= kQuickJsInterruptLimit) {
        vm->limit_exceeded = true;
        return 1;
    }
    return 0;
}

std::string JsVm::exception_text() {
    JSValue exception = JS_GetException(context);
    std::string output = "JavaScript exception";
    JSValue stack = JS_GetPropertyStr(context, exception, "stack");
    if (!JS_IsException(stack) && !JS_IsUndefined(stack)) {
        size_t size = 0;
        const char *text = JS_ToCStringLen2(context, &size, stack, false);
        if (text) {
            output.assign(text, size);
            JS_FreeCString(context, text);
        }
    }
    JS_FreeValue(context, stack);
    if (output == "JavaScript exception") {
        size_t size = 0;
        const char *text = JS_ToCStringLen2(context, &size, exception, false);
        if (text) {
            output.assign(text, size);
            JS_FreeCString(context, text);
        }
    }
    JS_FreeValue(context, exception);
    return output;
}

std::string JsVm::promise_error_text(JSValueConst promise) {
    JSValue reason = JS_PromiseResult(context, promise);
    JSValue stack = JS_IsObject(reason)
        ? JS_GetPropertyStr(context, reason, "stack") : JS_UNDEFINED;
    if (JS_IsException(stack)) {
        JSValue ignored = JS_GetException(context);
        JS_FreeValue(context, ignored);
        stack = JS_UNDEFINED;
    }
    JSValueConst display = JS_IsUndefined(stack) ? reason : stack;
    size_t size = 0;
    const char *text = JS_ToCStringLen2(context, &size, display, false);
    std::string output = text ? std::string(text, size) : "JavaScript module rejected";
    if (text)
        JS_FreeCString(context, text);
    JS_FreeValue(context, stack);
    JS_FreeValue(context, reason);
    return output;
}

char *JsVm::normalize_module(JSContext *context, const char *base, const char *name,
                             void *opaque) {
    auto *vm = static_cast<JsVm *>(opaque);
    if (!name || !*name)
        return nullptr;
    const std::string_view importer = base && *base ? std::string_view(base)
                                                    : std::string_view(vm->main_relative);
    std::string relative;
    if (!resolve_project_include(importer, name, relative))
        return nullptr;
    const fs::path extension(relative);
    if (extension.extension().empty())
        relative += ".js";
    else if (extension.extension() != ".js")
        return nullptr;
    char *result = static_cast<char *>(js_malloc(context, relative.size() + 1));
    if (!result)
        return nullptr;
    std::memcpy(result, relative.data(), relative.size());
    result[relative.size()] = '\0';
    return result;
}

JSModuleDef *JsVm::load_module(JSContext *context, const char *name, void *opaque) {
    auto *vm = static_cast<JsVm *>(opaque);
    std::string source;
    std::string error;
    const std::string relative = name ? name : "";
    if (!vm->source(relative, source, error)) {
        vm->log_error(relative, error);
        JS_ThrowReferenceError(context, "%s", error.c_str());
        return nullptr;
    }
    const std::string previous = vm->active_source;
    vm->active_source = relative;
    vm->interrupt_ticks = 0;
    vm->limit_exceeded = false;
    JSValue module = JS_Eval(context, source.data(), source.size(), relative.c_str(),
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    vm->active_source = previous;
    if (JS_IsException(module)) {
        error = vm->exception_text();
        vm->log_error(relative, error);
        JS_ThrowReferenceError(context, "%s", error.c_str());
        return nullptr;
    }
    auto *definition = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(module));
    JS_FreeValue(context, module);
    return definition;
}

bool JsVm::run_jobs(std::string &error) {
    while (JS_IsJobPending(runtime)) {
        JSContext *job_context = nullptr;
        const int status = JS_ExecutePendingJob(runtime, &job_context);
        if (status < 0) {
            JSContext *saved_context = context;
            if (job_context)
                context = job_context;
            error = exception_text();
            context = saved_context;
            return false;
        }
    }
    return true;
}

bool JsVm::evaluate_module(std::string_view relative, std::string_view source_text,
                           std::string &error) {
    const std::string filename(relative);
    active_source = filename;
    interrupt_ticks = 0;
    limit_exceeded = false;
    JSValue compiled = JS_Eval(context, source_text.data(), source_text.size(),
                               filename.c_str(),
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
        error = exception_text();
        active_source.clear();
        return false;
    }
    main_module = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled));
    interrupt_ticks = 0;
    limit_exceeded = false;
    JSValue result = JS_EvalFunction(context, compiled);
    if (JS_IsException(result)) {
        error = exception_text();
        active_source.clear();
        return false;
    }
    if (!run_jobs(error)) {
        JS_FreeValue(context, result);
        active_source.clear();
        return false;
    }
    const auto promise_state = JS_PromiseState(context, result);
    if (promise_state == JS_PROMISE_REJECTED) {
        error = promise_error_text(result);
        JS_FreeValue(context, result);
        active_source.clear();
        return false;
    }
    if (promise_state == JS_PROMISE_PENDING) {
        error = "JavaScript main module did not finish evaluating";
        JS_FreeValue(context, result);
        active_source.clear();
        return false;
    }
    JS_FreeValue(context, result);
    main_exports = JS_GetModuleNamespace(context, main_module);
    if (JS_IsException(main_exports)) {
        error = exception_text();
        active_source.clear();
        return false;
    }
    active_source.clear();
    return true;
}

bool JsVm::initialize(std::string &error) {
    runtime = JS_NewRuntime();
    if (!runtime) {
        error = "Cannot create QuickJS-NG VM";
        return false;
    }
    JS_SetMemoryLimit(runtime, kVmMemoryLimit);
    JS_SetMaxStackSize(runtime, 1024 * 1024);
    JS_SetCanBlock(runtime, false);
    JS_SetInterruptHandler(runtime, interrupt, this);
    JS_SetModuleLoaderFunc(runtime, normalize_module, load_module, this);
    context = JS_NewContext(runtime);
    if (!context) {
        error = "Cannot create QuickJS-NG context";
        return false;
    }
    JS_SetContextOpaque(context, this);
    JSValue global = JS_GetGlobalObject(context);
    JSValue card_api = JS_NewObject(context);
    js_set_api(context, card_api, "log", api_log, 1);
    js_set_api(context, card_api, "read", api_bus_read, 2);
    js_set_api(context, card_api, "write", api_bus_write, 3);
    js_set_api(context, card_api, "time_ns", api_time, 0);
    js_set_api(context, card_api, "signal_read", api_signal_read, 1);
    js_set_api(context, card_api, "signal_drive", api_signal_drive, 3);
    js_set_api(context, card_api, "on_signal", api_on_signal, 2);
    js_set_api(context, card_api, "after", api_after, 2);
    JS_SetPropertyStr(context, global, "card", card_api);
    JSValue project_api = JS_NewObject(context);
    js_set_api(context, project_api, "read", api_project_read, 1);
    js_set_api(context, project_api, "write", api_project_write, 2);
    JS_SetPropertyStr(context, global, "project", project_api);
    JS_SetPropertyStr(context, global, "state", JS_NewObject(context));
    JS_FreeValue(context, global);

    const auto main = source_cache.find(main_relative);
    if (main == source_cache.end()) {
        error = "Main script source was not loaded";
        return false;
    }
    if (!evaluate_module(main_relative, main->second, error)) {
        log_error(main_relative, error);
        return false;
    }
    return true;
}

JSValue JsVm::get_hook(const char *name) {
    if (!JS_IsUndefined(main_exports)) {
        JSValue function = JS_GetPropertyStr(context, main_exports, name);
        if (JS_IsFunction(context, function))
            return function;
        JS_FreeValue(context, function);
    }
    JSValue global = JS_GetGlobalObject(context);
    JSValue function = JS_GetPropertyStr(context, global, name);
    JS_FreeValue(context, global);
    return function;
}

bool JsVm::call_reset(bool cold, std::string &error) {
    CallbackScope callback(callback_depth);
    if (!callback.entered) {
        error = "script callback nesting limit exceeded (64)";
        return false;
    }
    active_source = main_relative;
    JSValue function = get_hook("on_reset");
    if (!JS_IsFunction(context, function)) {
        JS_FreeValue(context, function);
        return true;
    }
    JSValue argument = JS_NewBool(context, cold);
    interrupt_ticks = 0;
    limit_exceeded = false;
    JSValue result = JS_Call(context, function, JS_UNDEFINED, 1, &argument);
    JS_FreeValue(context, argument);
    JS_FreeValue(context, function);
    if (JS_IsException(result)) {
        error = exception_text();
        return false;
    }
    JS_FreeValue(context, result);
    return run_jobs(error);
}

bool JsVm::call_read(uint64_t address, uint8_t &value, std::string &error) {
    CallbackScope callback(callback_depth);
    if (!callback.entered) {
        error = "script callback nesting limit exceeded (64)";
        return false;
    }
    active_source = main_relative;
    JSValue function = get_hook("on_read");
    if (!JS_IsFunction(context, function)) {
        JS_FreeValue(context, function);
        value = 0;
        return true;
    }
    JSValue argument = JS_NewInt64(context, static_cast<int64_t>(address));
    interrupt_ticks = 0;
    limit_exceeded = false;
    JSValue result = JS_Call(context, function, JS_UNDEFINED, 1, &argument);
    JS_FreeValue(context, argument);
    JS_FreeValue(context, function);
    if (JS_IsException(result)) {
        error = exception_text();
        return false;
    }
    if (JS_IsUndefined(result)) {
        value = 0;
    } else {
        int64_t byte = 0;
        if (!js_integer(context, result, byte) || byte < 0 || byte > 255) {
            error = "on_read must return an integer byte from 0 to 255";
            JS_FreeValue(context, result);
            return false;
        }
        value = static_cast<uint8_t>(byte);
    }
    JS_FreeValue(context, result);
    return run_jobs(error);
}

bool JsVm::call_write(uint64_t address, uint8_t value, std::string &error) {
    CallbackScope callback(callback_depth);
    if (!callback.entered) {
        error = "script callback nesting limit exceeded (64)";
        return false;
    }
    active_source = main_relative;
    JSValue function = get_hook("on_write");
    if (!JS_IsFunction(context, function)) {
        JS_FreeValue(context, function);
        return true;
    }
    JSValue arguments[2] = {
        JS_NewInt64(context, static_cast<int64_t>(address)),
        JS_NewInt32(context, value)
    };
    interrupt_ticks = 0;
    limit_exceeded = false;
    JSValue result = JS_Call(context, function, JS_UNDEFINED, 2, arguments);
    JS_FreeValue(context, arguments[0]);
    JS_FreeValue(context, arguments[1]);
    JS_FreeValue(context, function);
    if (JS_IsException(result)) {
        error = exception_text();
        return false;
    }
    JS_FreeValue(context, result);
    return run_jobs(error);
}

uint64_t JsVm::retain_callback(int function_index, JSValueConst js_function) {
    (void)function_index;
    if (!JS_IsFunction(context, js_function))
        return 0;
    const uint64_t id = next_callback_id++;
    js_callbacks.emplace(id, JS_DupValue(context, js_function));
    return id;
}

void JsVm::release_function(uint64_t id) {
    const auto found = js_callbacks.find(id);
    if (found == js_callbacks.end())
        return;
    JS_FreeValue(context, found->second);
    js_callbacks.erase(found);
}

bool JsVm::invoke_callback(uint64_t id, std::string_view kind, std::string_view name,
                           int64_t value, std::string &error) {
    CallbackScope callback(callback_depth);
    if (!callback.entered) {
        error = "script callback nesting limit exceeded (64)";
        return false;
    }
    active_source = main_relative;
    const auto found = js_callbacks.find(id);
    if (found == js_callbacks.end())
        return true;
    JSValue function = JS_DupValue(context, found->second);
    JSValue arguments[2];
    int count = 1;
    if (kind == "signal") {
        arguments[0] = JS_NewStringLen(context, name.data(), name.size());
        arguments[1] = JS_NewInt64(context, value);
        count = 2;
    } else {
        arguments[0] = JS_NewInt64(context, value);
    }
    interrupt_ticks = 0;
    limit_exceeded = false;
    JSValue result = JS_Call(context, function, JS_UNDEFINED, count, arguments);
    for (int i = 0; i < count; ++i)
        JS_FreeValue(context, arguments[i]);
    JS_FreeValue(context, function);
    if (JS_IsException(result)) {
        error = exception_text();
        return false;
    }
    JS_FreeValue(context, result);
    return run_jobs(error);
}

bool JsVm::save_user_state(std::string &saved, std::string &error) {
    JSValue global = JS_GetGlobalObject(context);
    JSValue state = JS_GetPropertyStr(context, global, "state");
    JSValue json = JS_GetPropertyStr(context, global, "JSON");
    JSValue stringify = JS_GetPropertyStr(context, json, "stringify");
    JSValue result = JS_Call(context, stringify, json, 1, &state);
    JS_FreeValue(context, stringify);
    JS_FreeValue(context, json);
    JS_FreeValue(context, state);
    JS_FreeValue(context, global);
    if (JS_IsException(result)) {
        error = exception_text();
        return false;
    }
    size_t size = 0;
    const char *text = JS_ToCStringLen2(context, &size, result, false);
    if (!text) {
        JS_FreeValue(context, result);
        error = "global state could not be encoded as JSON";
        return false;
    }
    if (size > kStateLimit) {
        JS_FreeCString(context, text);
        JS_FreeValue(context, result);
        error = "saved state exceeds 1 MiB";
        return false;
    }
    saved.assign(text, size);
    JS_FreeCString(context, text);
    JS_FreeValue(context, result);
    try {
        const Json value = Json::parse(saved);
        if (!value.is_object()) {
            error = "global state must be a JSON object";
            return false;
        }
    } catch (...) {
        error = "global state must be a JSON object";
        return false;
    }
    return true;
}

bool JsVm::load_user_state(std::string_view saved, std::string &error) {
    Json validated;
    try {
        validated = Json::parse(saved.begin(), saved.end());
    } catch (...) {
        error = "saved state is malformed JSON";
        return false;
    }
    if (!validated.is_object() || saved.size() > kStateLimit) {
        error = "saved state must be a JSON object of at most 1 MiB";
        return false;
    }
    JSValue global = JS_GetGlobalObject(context);
    JSValue json = JS_GetPropertyStr(context, global, "JSON");
    JSValue parse = JS_GetPropertyStr(context, json, "parse");
    JSValue text = JS_NewStringLen(context, saved.data(), saved.size());
    JSValue state = JS_Call(context, parse, json, 1, &text);
    JS_FreeValue(context, text);
    JS_FreeValue(context, parse);
    JS_FreeValue(context, json);
    if (JS_IsException(state)) {
        JS_FreeValue(context, global);
        error = exception_text();
        return false;
    }
    if (JS_SetPropertyStr(context, global, "state", state) < 0) {
        JS_FreeValue(context, global);
        error = "cannot replace script state";
        return false;
    }
    JS_FreeValue(context, global);
    return true;
}

JSValue JsVm::api_log(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
    auto *vm = from(context);
    if (argc < 1)
        return js_error(context, "card.log expects a message");
    try {
        std::string text, error;
        if (!js_string(context, argv[0], text, error))
            return js_error(context, error);
        vm->card.log("script [" + vm->active_source + "]: " + text);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowInternalError(context, "script log failed");
    }
}

JSValue JsVm::api_bus_read(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
    auto *vm = from(context);
    if (argc < 2)
        return js_error(context, "card.read expects a space name and address");
    try {
        std::string name, error;
        int64_t address = 0;
        if (!js_string(context, argv[0], name, error) ||
            !js_integer(context, argv[1], address) || address < 0)
            return js_error(context, "space name or address is invalid");
        SrhHandle space = 0;
        uint8_t value = 0;
        if (!vm->card.lookup_space(name, space) || !vm->card.host->read ||
            vm->card.host->read(vm->card.host->context, vm->card.owner, space,
                                static_cast<uint64_t>(address), &value) != SRH_OK)
            return js_error(context, "bus read failed");
        return JS_NewInt32(context, value);
    } catch (...) {
        return JS_ThrowInternalError(context, "bus read failed");
    }
}

JSValue JsVm::api_bus_write(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
    auto *vm = from(context);
    if (argc < 3)
        return js_error(context, "card.write expects a space name, address, and byte");
    try {
        std::string name, error;
        int64_t address = 0, byte = 0;
        if (!js_string(context, argv[0], name, error) ||
            !js_integer(context, argv[1], address) || address < 0 ||
            !js_integer(context, argv[2], byte) || byte < 0 || byte > 255)
            return js_error(context, "space name, address, or byte is invalid");
        SrhHandle space = 0;
        if (!vm->card.lookup_space(name, space) || !vm->card.host->write ||
            vm->card.host->write(vm->card.host->context, vm->card.owner, space,
                                 static_cast<uint64_t>(address),
                                 static_cast<uint8_t>(byte)) != SRH_OK)
            return js_error(context, "bus write failed");
        return JS_TRUE;
    } catch (...) {
        return JS_ThrowInternalError(context, "bus write failed");
    }
}

JSValue JsVm::api_time(JSContext *context, JSValueConst, int, JSValueConst *) {
    auto *vm = from(context);
    uint64_t now = 0;
    if (!vm->card.host->time_ns ||
        vm->card.host->time_ns(vm->card.host->context, &now) != SRH_OK)
        return js_error(context, "simulation time is unavailable");
    return JS_NewInt64(context, static_cast<int64_t>(now));
}

JSValue JsVm::api_signal_read(JSContext *context, JSValueConst, int argc,
                              JSValueConst *argv) {
    auto *vm = from(context);
    if (argc < 1)
        return js_error(context, "card.signal_read expects a signal name");
    try {
        std::string name, error;
        if (!js_string(context, argv[0], name, error))
            return js_error(context, error);
        SrhHandle signal = 0;
        int32_t millivolts = 0;
        if (!vm->card.lookup_signal(name, signal) || !vm->card.host->signal_read ||
            vm->card.host->signal_read(vm->card.host->context, signal, &millivolts) != SRH_OK)
            return js_error(context, "signal read failed");
        return JS_NewInt32(context, millivolts);
    } catch (...) {
        return JS_ThrowInternalError(context, "signal read failed");
    }
}

JSValue JsVm::api_signal_drive(JSContext *context, JSValueConst, int argc,
                               JSValueConst *argv) {
    auto *vm = from(context);
    if (argc < 2)
        return js_error(context, "card.signal_drive expects a signal name and millivolts");
    try {
        std::string name, error;
        int64_t millivolts = 0, strength = 0;
        if (!js_string(context, argv[0], name, error) ||
            !js_integer(context, argv[1], millivolts) ||
            (argc >= 3 && !JS_IsUndefined(argv[2]) &&
             !js_integer(context, argv[2], strength)) ||
            millivolts < INT32_MIN || millivolts > INT32_MAX ||
            strength < INT32_MIN || strength > INT32_MAX)
            return js_error(context, "signal value or strength is invalid");
        SrhHandle signal = 0;
        if (!vm->card.lookup_signal(name, signal) || !vm->card.host->signal_drive ||
            vm->card.host->signal_drive(vm->card.host->context, vm->card.owner, signal,
                                        static_cast<int32_t>(millivolts),
                                        static_cast<int32_t>(strength)) != SRH_OK)
            return js_error(context, "signal drive failed");
        return JS_TRUE;
    } catch (...) {
        return JS_ThrowInternalError(context, "signal drive failed");
    }
}

JSValue JsVm::api_on_signal(JSContext *context, JSValueConst, int argc,
                            JSValueConst *argv) {
    auto *vm = from(context);
    if (argc < 2 || !JS_IsFunction(context, argv[1]))
        return js_error(context, "card.on_signal expects a signal name and callback");
    try {
        std::string name, error;
        if (!js_string(context, argv[0], name, error) ||
            !vm->register_signal(name, 2, error, argv[1]))
            return js_error(context, error.empty() ? "signal subscription failed" : error);
        return JS_TRUE;
    } catch (...) {
        return JS_ThrowInternalError(context, "signal subscription failed");
    }
}

JSValue JsVm::api_after(JSContext *context, JSValueConst, int argc,
                        JSValueConst *argv) {
    auto *vm = from(context);
    if (argc < 2 || !JS_IsFunction(context, argv[1]))
        return js_error(context, "card.after expects a delay and callback");
    int64_t delay = 0;
    if (!js_integer(context, argv[0], delay) || delay < 0)
        return js_error(context, "timer delay must be a non-negative integer");
    const uint64_t id = vm->register_timer(static_cast<uint64_t>(delay), 2, argv[1]);
    return id ? JS_NewInt64(context, static_cast<int64_t>(id))
              : js_error(context, "cannot schedule timer");
}

JSValue JsVm::api_project_read(JSContext *context, JSValueConst, int argc,
                               JSValueConst *argv) {
    auto *vm = from(context);
    if (argc < 1)
        return js_error(context, "project.read expects a relative path");
    try {
        std::string path, error;
        if (!js_string(context, argv[0], path, error))
            return js_error(context, error);
        std::vector<uint8_t> bytes;
        const SrhStatus status = vm->card.read_project(path, bytes);
        if (status != SRH_OK)
            return js_error(context, ScriptVm::status_text(status));
        const char *data = bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data());
        std::string normalized;
        if (normalize_project_path(path, normalized))
            vm->file_snapshots[normalized] = file_fingerprint({data, bytes.size()});
        return JS_NewStringLen(context, data, bytes.size());
    } catch (...) {
        return JS_ThrowInternalError(context, "project.read failed");
    }
}

JSValue JsVm::api_project_write(JSContext *context, JSValueConst, int argc,
                                JSValueConst *argv) {
    auto *vm = from(context);
    if (argc < 2)
        return js_error(context, "project.write expects a relative path and data string");
    try {
        std::string path, error;
        if (!js_string(context, argv[0], path, error))
            return js_error(context, error);
        size_t size = 0;
        const char *bytes = JS_ToCStringLen2(context, &size, argv[1], false);
        if (!bytes)
            return js_error(context, "project.write data must be a string");
        if (size > kFileLimit) {
            JS_FreeCString(context, bytes);
            return js_error(context, "project.write data exceeds 16 MiB");
        }
        const SrhStatus status = vm->card.write_project(
            path, reinterpret_cast<const uint8_t *>(bytes), size);
        JS_FreeCString(context, bytes);
        if (status != SRH_OK)
            return js_error(context, ScriptVm::status_text(status));
        return JS_TRUE;
    } catch (...) {
        return JS_ThrowInternalError(context, "project.write failed");
    }
}

} // namespace srz80_script
