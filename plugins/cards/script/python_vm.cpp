#include "script_card.h"

extern "C" int srz80_script_python_budget_step(void *context) noexcept {
    auto *vm = static_cast<srz80_script::PythonVm *>(context);
    if (!vm || vm->callback_depth == 0) return 0;
    if (vm->instructions_remaining == 0) return 1;
    --vm->instructions_remaining;
    return 0;
}

namespace srz80_script {
namespace {
bool initialized = false;
bool slots[16]{};

// Retained callbacks live in one registry dictionary instead of one global per
// callback, so a timer that reschedules itself does not grow the VM globals.
constexpr const char *kCallbackTable = "__srz80_callbacks";

PythonVm *current() { return static_cast<PythonVm *>(py_getvmctx()); }
py_Ref callback_table() {
    py_Ref table = py_getglobal(py_name(kCallbackTable));
    return table && py_isdict(table) ? table : nullptr;
}
std::string exception() {
    char *message = py_formatexc();
    std::string text = message ? message : "Python exception";
    py_free(message);
    py_clearexc(nullptr);
    return text;
}
bool text_arg(py_Ref value, std::string &out) {
    if (!py_isstr(value)) return TypeError("expected str");
    int length = 0;
    const char *data = py_tostrn(value, &length);
    out.assign(data, static_cast<size_t>(length));
    return true;
}
bool integer_arg(py_Ref value, uint64_t &out) {
    if (!py_isint(value) || py_toint(value) < 0)
        return ValueError("expected non-negative int");
    out = static_cast<uint64_t>(py_toint(value));
    return true;
}
bool status(SrhStatus result) {
    return result == SRH_OK || RuntimeError("%s", ScriptVm::status_text(result));
}
bool log(int argc, py_Ref argv) {
    if (argc != 1) return TypeError("log(message)");
    std::string message;
    if (!text_arg(py_arg(0), message) || !status(current()->card.log(message))) return false;
    py_newnone(py_retval()); return true;
}
bool bus_read(int argc, py_Ref argv) {
    if (argc != 2) return TypeError("read(space, address)");
    std::string name; uint64_t address = 0;
    if (!text_arg(py_arg(0), name) || !integer_arg(py_arg(1), address)) return false;
    SrhHandle space = 0;
    auto &card = current()->card;
    if (!card.lookup_space(name, space)) return ValueError("space not found");
    uint8_t value = 0;
    if (!card.host->read || !status(card.host->read(card.host->context, card.owner, space, address, &value))) return false;
    py_newint(py_retval(), value);
    return true;
}
bool bus_write(int argc, py_Ref argv) {
    if (argc != 3) return TypeError("write(space, address, byte)");
    std::string name; uint64_t address = 0, byte = 0;
    if (!text_arg(py_arg(0), name) || !integer_arg(py_arg(1), address) ||
        !integer_arg(py_arg(2), byte)) return false;
    if (byte > 255) return ValueError("byte out of range");
    SrhHandle space = 0; auto &card = current()->card;
    if (!card.lookup_space(name, space)) return ValueError("space not found");
    if (!card.host->write || !status(card.host->write(card.host->context, card.owner, space, address, static_cast<uint8_t>(byte)))) return false;
    py_newnone(py_retval()); return true;
}
bool time_ns(int argc, py_Ref) {
    if (argc) return TypeError("time_ns()");
    uint64_t time = 0; auto &card = current()->card;
    if (!card.host->time_ns || !status(card.host->time_ns(card.host->context, &time))) return false;
    py_newint(py_retval(), static_cast<int64_t>(time)); return true;
}
bool after(int argc, py_Ref argv) {
    if (argc != 2) return TypeError("after(delay_ns, callback)");
    uint64_t delay = 0;
    if (!integer_arg(py_arg(0), delay)) return false;
    auto *vm = current();
    const uint64_t id = vm->retain(py_arg(1));
    if (!vm->register_retained_timer(delay, id)) return RuntimeError("cannot schedule timer");
    py_newint(py_retval(), static_cast<int64_t>(id)); return true;
}
bool project_read(int argc, py_Ref argv) {
    if (argc != 1) return TypeError("read(path)");
    std::string path;
    if (!text_arg(py_arg(0), path)) return false;
    std::vector<uint8_t> bytes;
    auto *vm = current();
    if (!status(vm->card.read_project(path, bytes))) return false;
    std::string normalized;
    if (normalize_project_path(path, normalized))
        vm->file_snapshots[normalized] = file_fingerprint({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
    auto *target = py_newbytes(py_retval(), static_cast<int>(bytes.size()));
    if (!bytes.empty()) std::memcpy(target, bytes.data(), bytes.size());
    return true;
}
bool project_write(int argc, py_Ref argv) {
    if (argc != 2) return TypeError("write(path, bytes)");
    std::string path;
    if (!text_arg(py_arg(0), path)) return false;
    if (!py_istype(py_arg(1), tp_bytes)) return TypeError("expected bytes");
    int size = 0; const auto *data = py_tobytes(py_arg(1), &size);
    if (!status(current()->card.write_project(path, data, static_cast<uint64_t>(size)))) return false;
    py_newbool(py_retval(), true); return true;
}
void bind_module(const char *name, const char *method, py_CFunction function) {
    py_Ref module = py_getmodule(name);
    if (!module) module = py_newmodule(name);
    py_bindfunc(module, method, function);
    py_setglobal(py_name(name), module);
}
} // namespace

PythonVm::~PythonVm() {
    if (slot >= 0) {
        py_switchvm(slot);
        py_resetvm();
        slots[slot] = false;
    }
}
bool PythonVm::initialize(std::string &error) {
    if (!initialized) { py_initialize(); initialized = true; slots[0] = true; }
    for (int i = 1; i < 16; ++i) if (!slots[i]) { slot = i; slots[i] = true; break; }
    if (slot < 0) { error = "PocketPy supports at most 15 script cards"; return false; }
    py_switchvm(slot); py_resetvm(); py_setvmctx(this);
    CallbackScope scope(callback_depth);
    instructions_remaining = card.python_instruction_limit;
    py_newdict(py_r0());
    py_setglobal(py_name(kCallbackTable), py_r0());
    bind_module("card", "log", log);
    bind_module("card", "read", bus_read);
    bind_module("card", "write", bus_write);
    bind_module("card", "time_ns", time_ns);
    bind_module("card", "after", after);
    bind_module("project", "read", project_read);
    bind_module("project", "write", project_write);
    std::string source_text;
    if (!source(main_relative, source_text, error)) return false;
    if (source_text.find('\0') != std::string::npos ||
        !py_exec(source_text.c_str(), main_relative.c_str(), EXEC_MODE, nullptr)) {
        error = source_text.find('\0') != std::string::npos ? "NUL in Python source" : exception();
        return false;
    }
    return true;
}
bool PythonVm::call(py_Ref function, const std::vector<int64_t> &args, std::string &error) {
    py_switchvm(slot);
    if (!function || py_isnil(function)) return true;
    CallbackScope scope(callback_depth);
    if (!scope.entered) { error = "callback nesting limit exceeded"; return false; }
    if (callback_depth == 1) instructions_remaining = card.python_instruction_limit;
    py_push(function); py_pushnil();
    for (auto arg : args) { py_newint(py_r0(), arg); py_push(py_r0()); }
    if (!py_vectorcall(static_cast<uint16_t>(args.size()), 0)) { error = exception(); return false; }
    return true;
}
bool PythonVm::call_named(const char *name, const std::vector<int64_t> &args,
                          std::string &error) {
    py_switchvm(slot);
    return call(py_getglobal(py_name(name)), args, error);
}
bool PythonVm::call_reset(bool cold, std::string &error) {
    return call_named("on_reset", {cold ? 1 : 0}, error);
}
bool PythonVm::call_read(uint64_t address, uint8_t &value, std::string &error) {
    if (!call_named("on_read", {static_cast<int64_t>(address)}, error)) return false;
    py_Ref result = py_retval();
    if (!py_isint(result) || py_toint(result) < 0 || py_toint(result) > 255) {
        error = "on_read must return a byte"; return false;
    }
    value = static_cast<uint8_t>(py_toint(result)); return true;
}
bool PythonVm::call_write(uint64_t address, uint8_t value, std::string &error) {
    return call_named("on_write", {static_cast<int64_t>(address), value}, error);
}
uint64_t PythonVm::retain(py_Ref callback) {
    py_switchvm(slot);
    py_Ref table = callback_table();
    if (!table) return 0;
    const uint64_t id = next_callback_id++;
    if (!py_dict_setitem_by_int(table, static_cast<py_i64>(id), callback)) {
        py_clearexc(nullptr);
        return 0;
    }
    return id;
}
void PythonVm::release_function(uint64_t id) {
    if (slot < 0) return;
    py_switchvm(slot);
    py_Ref table = callback_table();
    if (!table) return;
    const py_i64 key = static_cast<py_i64>(id);
    if (py_dict_getitem_by_int(table, key) == 1)
        py_dict_delitem_by_int(table, key);
    py_clearexc(nullptr);
}
bool PythonVm::invoke_callback(uint64_t id, std::string_view, std::string_view,
                               int64_t value, std::string &error) {
    if (slot < 0) return true;
    py_switchvm(slot);
    py_Ref table = callback_table();
    if (!table) return true;
    if (py_dict_getitem_by_int(table, static_cast<py_i64>(id)) != 1) return true;
    return call(py_retval(), {value}, error);
}
bool PythonVm::save_user_state(std::string &out, std::string &error) {
    py_switchvm(slot);
    py_Ref state = py_getglobal(py_name("state"));
    if (!state || py_isnil(state)) { out = "{}"; return true; }
    if (!py_isdict(state) || !py_json_dumps(state, 0)) {
        error = py_checkexc() ? exception() : "state must be a JSON object"; return false;
    }
    out = py_tostr(py_retval());
    if (out.size() > kStateLimit) { error = "state exceeds 1 MiB"; return false; }
    return true;
}
bool PythonVm::load_user_state(std::string_view data, std::string &error) {
    py_switchvm(slot);
    const std::string text(data);
    if (!py_json_loads(text.c_str())) { error = exception(); return false; }
    if (!py_isdict(py_retval())) { error = "state must be a JSON object"; return false; }
    py_setglobal(py_name("state"), py_retval());
    return true;
}
} // namespace srz80_script
