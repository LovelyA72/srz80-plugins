// Minimal MAME compatibility surface for the imported G65816 instruction core.
// It intentionally provides only CPU-local state and a byte bus; SRZ80 owns
// scheduling, address mapping, debugging, and persistence.
#pragma once

#include <cstdarg>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using offs_t = uint32_t;

namespace util {
class disasm_interface {
public:
    class data_buffer {
    public:
        data_buffer() = default;
        explicit data_buffer(const std::vector<u8> &bytes, offs_t base = 0) : bytes_(&bytes), base_(base) {}
        u8 r8(offs_t address) const {
            const auto offset = address - base_;
            return !bytes_ || address < base_ || offset >= bytes_->size() ? 0 : (*bytes_)[offset];
        }
        u16 r16(offs_t address) const { return u16(r8(address) | (u16(r8(address + 1)) << 8)); }
        u32 r32(offs_t address) const { return u32(r16(address)) | (u32(r16(address + 2)) << 16); }
    private:
        const std::vector<u8> *bytes_ = nullptr;
        offs_t base_ = 0;
    };
    static constexpr offs_t SUPPORTED = 0x80000000U, STEP_OVER = 0x40000000U,
                           STEP_OUT = 0x20000000U, STEP_COND = 0x10000000U, PAGED = 1;
    virtual ~disasm_interface() = default;
    virtual u32 opcode_alignment() const { return 1; }
    virtual u32 interface_flags() const { return 0; }
    virtual u32 page_address_bits() const { return 0; }
    virtual offs_t disassemble(std::ostream &, offs_t, const data_buffer &, const data_buffer &) { return 1; }
};
inline std::string string_format(const char *format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return buffer;
}
inline void stream_format(std::ostream &stream, const char *format, ...) {
    char buffer[128];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    stream << buffer;
}
} // namespace util
using util::string_format;

enum endianness_t { ENDIANNESS_LITTLE };
using device_type = int;
class machine_config {};
class device_t {};
class address_map {
public:
    address_map &operator()(uint32_t, uint32_t) { return *this; }
    address_map &mirror(uint32_t) { return *this; }
    template<class T> address_map &w(T) { return *this; }
    template<class T> address_map &r(T) { return *this; }
};
class address_map_constructor { public: address_map_constructor() = default; template<class T> address_map_constructor(T, void *) {} };

class cpu_device;
class address_space {
public:
    explicit address_space(cpu_device *owner = nullptr) : owner_(owner) {}
    u8 read_byte(uint32_t address) const;
    u16 read_word(uint32_t address) const;
    void write_byte(uint32_t address, u8 value) const;
    template<class T> void cache(T &target) { target.bind(this); }
    template<class T> void specific(T &target) { target.bind(this); }
    template<class... T> void install_write_handler(T &&...) {}
    template<class... T> void install_read_handler(T &&...) {}
private:
    cpu_device *owner_;
};

template<unsigned, unsigned, unsigned, endianness_t> class memory_access {
public:
    class cache {
    public:
        void bind(const address_space *space) { space_ = space; }
        u8 read_byte(uint32_t address) const { return space_->read_byte(address); }
    protected: const address_space *space_ = nullptr;
    };
    class specific : public cache {
    public:
        void write_byte(uint32_t address, u8 value) const { this->space_->write_byte(address, value); }
    };
};

class address_space_config {
public:
    address_space_config(const char *, endianness_t, int, int, int, address_map_constructor = {}) {}
};
class device_memory_interface { public: using space_config_vector = std::vector<std::pair<int, const address_space_config *>>; };
class device_state_entry { public: int index() const { return 0; } };
class noop_state {
public:
    noop_state &callimport() { return *this; }
    noop_state &callexport() { return *this; }
    noop_state &formatstr(const char *) { return *this; }
    noop_state &mask(uint32_t) { return *this; }
    noop_state &noshow() { return *this; }
};
class noop_save { public: template<class T> void register_postload(T) {} };
class noop_machine { public: noop_save &save() { return save_; } private: noop_save save_; };
class devcb_write8 {
public:
    explicit devcb_write8(device_t &) {}
    struct binder {};
    binder bind() { return {}; }
    void operator()(u8) const {}
};

class cpu_device : public device_t {
public:
    using space_config_vector = device_memory_interface::space_config_vector;
    cpu_device(const machine_config &, device_type, const char *, device_t *, uint32_t) : spaces_{address_space(this), address_space(this), address_space(this), address_space(this)} {}
    virtual ~cpu_device() = default;
    void set_bus(std::function<u8(uint32_t)> read, std::function<void(uint32_t, u8)> write) { read_ = std::move(read); write_ = std::move(write); }
    u8 bus_read(uint32_t address) const { return read_ ? read_(address & 0xffffff) : 0xff; }
    void bus_write(uint32_t address, u8 value) const { if (write_) write_(address & 0xffffff, value); }
    address_space &space(int index) { return spaces_[index < 0 || index >= 4 ? 0 : index]; }
    bool has_space(int) const { return false; }
    bool has_configured_map(int) const { return false; }
    virtual uint32_t execute_min_cycles() const noexcept { return 1; }
    virtual uint32_t execute_max_cycles() const noexcept { return 1; }
    virtual void execute_run() {}
    virtual void execute_set_input(int, int) {}
    virtual device_memory_interface::space_config_vector memory_space_config() const { return {}; }
    virtual void device_start() {}
    virtual void device_reset() {}
    virtual void state_import(const device_state_entry &) {}
    virtual void state_export(const device_state_entry &) {}
    virtual void state_string_export(const device_state_entry &, std::string &) const {}
    virtual std::unique_ptr<util::disasm_interface> create_disassembler() { return {}; }
    noop_state state_add(int, const char *, uint32_t &) { return {}; }
    noop_state state_add(int, const char *, uint16_t &) { return {}; }
    void set_icountptr(int &) {}
    void debugger_instruction_hook(uint32_t) {}
    void debugger_wait_hook() {}
    void standard_irq_callback(int, uint32_t) {}
    template<class T> void save_item(T &) {}
    noop_machine &machine() { return machine_; }
private:
    std::function<u8(uint32_t)> read_;
    std::function<void(uint32_t, u8)> write_;
    address_space spaces_[4];
    noop_machine machine_;
};
inline u8 address_space::read_byte(uint32_t address) const { return owner_->bus_read(address); }
inline u16 address_space::read_word(uint32_t address) const { return u16(read_byte(address) | (u16(read_byte(address + 1)) << 8)); }
inline void address_space::write_byte(uint32_t address, u8 value) const { owner_->bus_write(address, value); }

#define ATTR_COLD
#define DEFINE_DEVICE_TYPE(...)
#define DECLARE_DEVICE_TYPE(...)
#define FUNC(x) &x
#define NAME(x) x
#define STATE_GENPC 1000
#define STATE_GENPCBASE 1001
#define STATE_GENFLAGS 1002
#define AS_PROGRAM 0
#define AS_DATA 1
#define AS_OPCODES 2
#define CLEAR_LINE 0
#define ASSERT_LINE 1
#define HOLD_LINE 2
inline constexpr device_type G65816 = 1, G65802 = 2, _5A22 = 3;
template<class T> int save_prepost_delegate(T, void *) { return 0; }
template<class... T> int write8smo_delegate(T &&...) { return 0; }
template<class... T> int read8smo_delegate(T &&...) { return 0; }
