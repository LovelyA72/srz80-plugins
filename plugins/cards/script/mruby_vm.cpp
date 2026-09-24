#include "script_card.h"

#include <mruby/array.h>
#include <mruby/compile.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/variable.h>

namespace srz80_script {
namespace {

std::string string_value(mrb_state *mrb, mrb_value value) {
    if (!mrb_string_p(value))
        mrb_raise(mrb, E_TYPE_ERROR, "expected String");
    return {RSTRING_PTR(value), static_cast<size_t>(RSTRING_LEN(value))};
}

uint64_t nonnegative(mrb_state *mrb, mrb_value value) {
    if (!mrb_integer_p(value) || mrb_integer(value) < 0)
        mrb_raise(mrb, E_ARGUMENT_ERROR, "expected non-negative Integer");
    return static_cast<uint64_t>(mrb_integer(value));
}

void check_status(mrb_state *mrb, SrhStatus status) {
    if (status != SRH_OK)
        mrb_raise(mrb, E_RUNTIME_ERROR, ScriptVm::status_text(status));
}

mrb_value api_log(mrb_state *mrb, mrb_value) {
    mrb_value arg;
    mrb_get_args(mrb, "o", &arg);
    check_status(mrb, MrubyVm::from(mrb)->card.log(string_value(mrb, arg)));
    return mrb_nil_value();
}

mrb_value api_read(mrb_state *mrb, mrb_value) {
    mrb_value space, address;
    mrb_get_args(mrb, "oo", &space, &address);
    auto *vm = MrubyVm::from(mrb);
    SrhHandle handle = 0;
    if (!vm->card.lookup_space(string_value(mrb, space), handle))
        mrb_raise(mrb, E_ARGUMENT_ERROR, "space not found");
    if (!vm->card.host->read)
        mrb_raise(mrb, E_RUNTIME_ERROR, "bus read is unavailable");
    uint8_t value = 0;
    check_status(mrb, vm->card.host->read(vm->card.host->context, vm->card.owner, handle,
                                          nonnegative(mrb, address), &value));
    return mrb_fixnum_value(value);
}

mrb_value api_write(mrb_state *mrb, mrb_value) {
    mrb_value space, address, byte;
    mrb_get_args(mrb, "ooo", &space, &address, &byte);
    auto *vm = MrubyVm::from(mrb);
    SrhHandle handle = 0;
    if (!vm->card.lookup_space(string_value(mrb, space), handle))
        mrb_raise(mrb, E_ARGUMENT_ERROR, "space not found");
    if (!vm->card.host->write)
        mrb_raise(mrb, E_RUNTIME_ERROR, "bus write is unavailable");
    const uint64_t data = nonnegative(mrb, byte);
    if (data > 255)
        mrb_raise(mrb, E_ARGUMENT_ERROR, "byte out of range");
    check_status(mrb, vm->card.host->write(vm->card.host->context, vm->card.owner, handle,
                                           nonnegative(mrb, address), static_cast<uint8_t>(data)));
    return mrb_nil_value();
}

mrb_value api_time(mrb_state *mrb, mrb_value) {
    auto *vm = MrubyVm::from(mrb);
    uint64_t time = 0;
    if (!vm->card.host->time_ns)
        mrb_raise(mrb, E_RUNTIME_ERROR, "simulation time is unavailable");
    check_status(mrb, vm->card.host->time_ns(vm->card.host->context, &time));
    return mrb_fixnum_value(static_cast<mrb_int>(time));
}

mrb_value api_signal_read(mrb_state *mrb, mrb_value) {
    mrb_value name;
    mrb_get_args(mrb, "o", &name);
    auto *vm = MrubyVm::from(mrb);
    SrhHandle handle = 0;
    if (!vm->card.lookup_signal(string_value(mrb, name), handle))
        mrb_raise(mrb, E_ARGUMENT_ERROR, "signal not found");
    if (!vm->card.host->signal_read)
        mrb_raise(mrb, E_RUNTIME_ERROR, "signal read is unavailable");
    int32_t value = 0;
    check_status(mrb, vm->card.host->signal_read(vm->card.host->context, handle, &value));
    return mrb_fixnum_value(value);
}

mrb_value api_signal_drive(mrb_state *mrb, mrb_value) {
    mrb_value name, voltage, strength = mrb_fixnum_value(0);
    mrb_get_args(mrb, "oo|o", &name, &voltage, &strength);
    auto *vm = MrubyVm::from(mrb);
    SrhHandle handle = 0;
    if (!vm->card.lookup_signal(string_value(mrb, name), handle))
        mrb_raise(mrb, E_ARGUMENT_ERROR, "signal not found");
    if (!mrb_integer_p(voltage) || !mrb_integer_p(strength))
        mrb_raise(mrb, E_TYPE_ERROR, "expected Integer voltage and strength");
    if (mrb_integer(voltage) < INT32_MIN || mrb_integer(voltage) > INT32_MAX ||
        mrb_integer(strength) < INT32_MIN || mrb_integer(strength) > INT32_MAX)
        mrb_raise(mrb, E_ARGUMENT_ERROR, "signal level or strength is out of range");
    if (!vm->card.host->signal_drive)
        mrb_raise(mrb, E_RUNTIME_ERROR, "signal drive is unavailable");
    check_status(mrb, vm->card.host->signal_drive(vm->card.host->context, vm->card.owner,
        handle, static_cast<int32_t>(mrb_integer(voltage)),
        static_cast<int32_t>(mrb_integer(strength))));
    return mrb_nil_value();
}

mrb_value api_after(mrb_state *mrb, mrb_value) {
    mrb_value delay, callback;
    mrb_get_args(mrb, "oo", &delay, &callback);
    auto *vm = MrubyVm::from(mrb);
    const uint64_t id = vm->register_retained_timer(nonnegative(mrb, delay), vm->retain(callback));
    if (!id)
        mrb_raise(mrb, E_RUNTIME_ERROR, "cannot schedule timer");
    return mrb_fixnum_value(static_cast<mrb_int>(id));
}

mrb_value api_on_signal(mrb_state *mrb, mrb_value) {
    mrb_value name, callback;
    mrb_get_args(mrb, "oo", &name, &callback);
    auto *vm = MrubyVm::from(mrb);
    std::string error;
    if (!vm->register_retained_signal(string_value(mrb, name), vm->retain(callback), error))
        mrb_raise(mrb, E_RUNTIME_ERROR, error.c_str());
    return mrb_nil_value();
}

mrb_value api_project_read(mrb_state *mrb, mrb_value) {
    mrb_value path;
    mrb_get_args(mrb, "o", &path);
    auto *vm = MrubyVm::from(mrb);
    std::string name = string_value(mrb, path);
    std::vector<uint8_t> bytes;
    check_status(mrb, vm->card.read_project(name, bytes));
    const char *data = bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data());
    std::string normalized;
    if (normalize_project_path(name, normalized))
        vm->file_snapshots[normalized] = file_fingerprint({data, bytes.size()});
    return mrb_str_new(mrb, data, bytes.size());
}

mrb_value api_project_write(mrb_state *mrb, mrb_value) {
    mrb_value path, data;
    mrb_get_args(mrb, "oo", &path, &data);
    auto *vm = MrubyVm::from(mrb);
    std::string name = string_value(mrb, path);
    std::string bytes = string_value(mrb, data);
    check_status(mrb, vm->card.write_project(name,
        reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()));
    return mrb_true_value();
}

mrb_value api_require(mrb_state *mrb, mrb_value) {
    mrb_value path;
    mrb_get_args(mrb, "o", &path);
    auto *vm = MrubyVm::from(mrb);
    std::string requested = string_value(mrb, path);
    if (requested.empty() || requested.find('\0') != std::string::npos)
        mrb_raise(mrb, E_ARGUMENT_ERROR, "invalid require path");
    if (fs::path(requested).extension().empty()) requested += ".rb";
    if (fs::path(requested).extension() != ".rb")
        mrb_raise(mrb, E_ARGUMENT_ERROR, "require accepts only .rb modules");
    std::string relative;
    const std::string_view importer = vm->import_stack.empty()
        ? std::string_view(vm->main_relative) : std::string_view(vm->import_stack.back());
    if (!resolve_project_include(importer, requested, relative))
        mrb_raise(mrb, E_ARGUMENT_ERROR, "require path escapes the project or is invalid");
    if (!vm->required_paths.insert(relative).second) return mrb_false_value();
    std::string source, error;
    if (!vm->source(relative, source, error)) {
        vm->required_paths.erase(relative);
        mrb_raise(mrb, E_RUNTIME_ERROR, error.c_str());
    }
    vm->import_stack.push_back(relative);
    mrb_ccontext *context = mrbc_context_new(mrb);
    mrbc_filename(mrb, context, relative.c_str());
    mrb_load_nstring_cxt(mrb, source.data(), source.size(), context);
    mrbc_context_free(mrb, context);
    vm->import_stack.pop_back();
    if (mrb->exc) {
        vm->required_paths.erase(relative);
        return mrb_nil_value();
    }
    return mrb_true_value();
}

bool from_json(mrb_state *mrb, const Json &input, mrb_value &out, unsigned depth) {
    if (depth > 64) return false;
    if (input.is_null()) out = mrb_nil_value();
    else if (input.is_boolean()) out = mrb_bool_value(input.get<bool>());
    else if (input.is_number_integer()) out = mrb_fixnum_value(input.get<int64_t>());
    else if (input.is_number_float()) out = mrb_float_value(mrb, input.get<double>());
    else if (input.is_string()) {
        const auto text = input.get<std::string>();
        out = mrb_str_new(mrb, text.data(), text.size());
    } else if (input.is_array()) {
        out = mrb_ary_new(mrb);
        for (const auto &item : input) {
            mrb_value value;
            if (!from_json(mrb, item, value, depth + 1)) return false;
            mrb_ary_push(mrb, out, value);
        }
    } else if (input.is_object()) {
        out = mrb_hash_new(mrb);
        for (auto it = input.begin(); it != input.end(); ++it) {
            mrb_value value;
            if (!from_json(mrb, it.value(), value, depth + 1)) return false;
            mrb_hash_set(mrb, out, mrb_str_new(mrb, it.key().data(), it.key().size()), value);
        }
    } else return false;
    return true;
}

bool to_json(mrb_state *mrb, mrb_value value, Json &out, unsigned depth,
             std::set<const void *> &active) {
    if (depth > 64) return false;
    if (mrb_nil_p(value)) out = nullptr;
    else if (mrb_true_p(value) || mrb_false_p(value)) out = mrb_bool(value);
    else if (mrb_integer_p(value)) out = static_cast<int64_t>(mrb_integer(value));
    else if (mrb_float_p(value)) {
        if (!std::isfinite(mrb_float(value))) return false;
        out = static_cast<double>(mrb_float(value));
    } else if (mrb_string_p(value)) out = string_value(mrb, value);
    else if (mrb_array_p(value) || mrb_hash_p(value)) {
        const void *pointer = mrb_ptr(value);
        if (!active.insert(pointer).second) return false;
        if (mrb_array_p(value)) {
            out = Json::array();
            for (mrb_int i = 0; i < RARRAY_LEN(value); ++i) {
                Json item;
                if (!to_json(mrb, mrb_ary_ref(mrb, value, i), item, depth + 1, active)) return false;
                out.push_back(std::move(item));
            }
        } else {
            out = Json::object();
            mrb_value keys = mrb_hash_keys(mrb, value);
            for (mrb_int i = 0; i < RARRAY_LEN(keys); ++i) {
                mrb_value key = mrb_ary_ref(mrb, keys, i);
                if (!mrb_string_p(key)) return false;
                Json item;
                if (!to_json(mrb, mrb_hash_get(mrb, value, key), item, depth + 1, active)) return false;
                out[string_value(mrb, key)] = std::move(item);
            }
        }
        active.erase(pointer);
    } else return false;
    return true;
}

} // namespace

MrubyVm::~MrubyVm() {
    deactivate();
    if (state) mrb_close(state);
}

void MrubyVm::instruction_hook(mrb_state *mrb, const mrb_irep *, const mrb_code *, mrb_value *) {
    auto *vm = from(mrb);
    if (++vm->instructions > kLuaInstructionLimit)
        mrb_raise(mrb, E_RUNTIME_ERROR, "script instruction limit exceeded");
}

std::string MrubyVm::exception_text() {
    if (!state->exc) return "mruby operation failed";
    mrb_value text = mrb_obj_as_string(state, mrb_obj_value(state->exc));
    std::string result = string_value(state, text);
    state->exc = nullptr;
    return result;
}

bool MrubyVm::initialize(std::string &error) {
    state = mrb_open();
    if (!state || state->exc) {
        error = state ? exception_text() : "cannot create mruby VM";
        return false;
    }
    state->ud = this;
    current_source = main_relative;
    state->code_fetch_hook = instruction_hook;
    auto *card_module = mrb_define_module(state, "Card");
    auto *project_module = mrb_define_module(state, "Project");
    mrb_define_module_function(state, card_module, "log", api_log, MRB_ARGS_REQ(1));
    mrb_define_module_function(state, card_module, "read", api_read, MRB_ARGS_REQ(2));
    mrb_define_module_function(state, card_module, "write", api_write, MRB_ARGS_REQ(3));
    mrb_define_module_function(state, card_module, "time_ns", api_time, MRB_ARGS_NONE());
    mrb_define_module_function(state, card_module, "signal_read", api_signal_read, MRB_ARGS_REQ(1));
    mrb_define_module_function(state, card_module, "signal_drive", api_signal_drive, MRB_ARGS_ARG(2,1));
    mrb_define_module_function(state, card_module, "on_signal", api_on_signal, MRB_ARGS_REQ(2));
    mrb_define_module_function(state, card_module, "after", api_after, MRB_ARGS_REQ(2));
    mrb_define_module_function(state, project_module, "read", api_project_read, MRB_ARGS_REQ(1));
    mrb_define_module_function(state, project_module, "write", api_project_write, MRB_ARGS_REQ(2));
    mrb_define_method(state, state->kernel_module, "require", api_require, MRB_ARGS_REQ(1));
    mrb_gv_set(state, mrb_intern_lit(state, "$state"), mrb_hash_new(state));
    std::string text;
    if (!source(main_relative, text, error)) return false;
    mrb_ccontext *context = mrbc_context_new(state);
    instructions = 0;
    mrbc_filename(state, context, main_relative.c_str());
    mrb_load_nstring_cxt(state, text.data(), text.size(), context);
    mrbc_context_free(state, context);
    if (state->exc) { error = exception_text(); return false; }
    return true;
}

bool MrubyVm::call_hook(const char *name, mrb_int argc, mrb_value *argv,
                        mrb_value &result, std::string &error) {
    CallbackScope scope(callback_depth);
    if (!scope.entered) { error = "callback nesting limit exceeded"; return false; }
    if (callback_depth == 1) instructions = 0;
    mrb_value top = mrb_top_self(state);
    mrb_sym method = mrb_intern_cstr(state, name);
    if (!mrb_respond_to(state, top, method)) { result = mrb_nil_value(); return true; }
    result = mrb_funcall_argv(state, top, method, argc, argv);
    if (state->exc) { error = exception_text(); return false; }
    return true;
}

bool MrubyVm::call_reset(bool cold, std::string &error) {
    mrb_value arg = mrb_bool_value(cold), result;
    return call_hook("on_reset", 1, &arg, result, error);
}

bool MrubyVm::call_read(uint64_t address, uint8_t &value, std::string &error) {
    mrb_value arg = mrb_fixnum_value(address), result;
    if (!call_hook("on_read", 1, &arg, result, error)) return false;
    if (mrb_nil_p(result)) { value = 0; return true; }
    if (!mrb_integer_p(result) || mrb_integer(result) < 0 || mrb_integer(result) > 255) {
        error = "on_read must return a byte"; return false;
    }
    value = static_cast<uint8_t>(mrb_integer(result));
    return true;
}

bool MrubyVm::call_write(uint64_t address, uint8_t value, std::string &error) {
    mrb_value args[2] = {mrb_fixnum_value(address), mrb_fixnum_value(value)}, result;
    return call_hook("on_write", 2, args, result, error);
}

uint64_t MrubyVm::retain(mrb_value callback) {
    if (!mrb_proc_p(callback)) return 0;
    uint64_t id = next_callback_id++;
    mrb_gc_register(state, callback);
    callbacks.emplace(id, callback);
    return id;
}

void MrubyVm::release_function(uint64_t id) {
    auto found = callbacks.find(id);
    if (found != callbacks.end()) {
        mrb_gc_unregister(state, found->second);
        callbacks.erase(found);
    }
}

bool MrubyVm::invoke_callback(uint64_t id, std::string_view kind, std::string_view name,
                              int64_t value, std::string &error) {
    auto found = callbacks.find(id);
    if (found == callbacks.end()) { error = "callback not found"; return false; }
    CallbackScope scope(callback_depth);
    if (!scope.entered) { error = "callback nesting limit exceeded"; return false; }
    if (callback_depth == 1) instructions = 0;
    mrb_value args[2] = {mrb_str_new(state, name.data(), name.size()), mrb_fixnum_value(value)};
    mrb_funcall_argv(state, found->second, mrb_intern_lit(state, "call"),
                    kind == "signal" ? 2 : 1, kind == "signal" ? args : &args[1]);
    if (state->exc) { error = exception_text(); return false; }
    return true;
}

bool MrubyVm::save_user_state(std::string &saved, std::string &error) {
    mrb_value value = mrb_gv_get(state, mrb_intern_lit(state, "$state"));
    if (!mrb_hash_p(value)) { error = "$state must be a Hash"; return false; }
    Json result;
    std::set<const void *> active;
    if (!to_json(state, value, result, 0, active)) {
        error = "$state contains unsupported values or cycles"; return false;
    }
    saved = result.dump();
    return true;
}

bool MrubyVm::load_user_state(std::string_view saved, std::string &error) {
    Json input = Json::parse(saved, nullptr, false);
    if (!input.is_object()) { error = "script state must be a JSON object"; return false; }
    mrb_value value;
    if (!from_json(state, input, value, 0)) { error = "invalid script state"; return false; }
    mrb_gv_set(state, mrb_intern_lit(state, "$state"), value);
    return true;
}

} // namespace srz80_script
