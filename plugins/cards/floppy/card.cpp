#include <state.hpp>
// F1440 removable block-storage card.
//
// The card is a bus slave for its register block and a bus master while it
// performs DMA into RAM.  It is deliberately not an emulation of a PC floppy
// controller: there are no cylinders, heads, tracks, motor control, seek
// timing, or sector IDs.  The guest sees one removable 1.44 MB medium as a
// linear array of 2880 sectors, and the medium generation counter is the only
// way it learns that the user swapped disks.
//
// The host filename never reaches the guest.  Only this card knows that its
// medium came from a host file; the provider channel at the bottom of this file
// is the emulator-only boundary where a tool acts like a human inserting and
// removing disks.  Nothing above that boundary mentions a path.
#include <boundary.hpp>
#include <nlohmann/json.hpp>
#include <srz80/providers.h>
#include <srz80/signals.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
using Json = nlohmann::json;

// F1440 medium.  These numbers are the entire hardware specification of the
// medium type; the raw `.img`/`.ima` container convention belongs to the tool
// and the emulator, never to the guest-visible interface.
constexpr uint32_t kSectorSize = 512;
constexpr uint32_t kSectorCount = 2880;
constexpr uint64_t kCapacity = uint64_t(kSectorSize) * kSectorCount; // 1,474,560
// Internally useful description of the same medium; software never sees it.
constexpr uint32_t kTracks = 80;
constexpr uint32_t kHeads = 2;
constexpr uint32_t kSectorsPerTrack = 18;
static_assert(uint64_t(kTracks) * kHeads * kSectorsPerTrack == kSectorCount,
              "F1440 geometry must describe exactly the documented sector count");

// One command moves at most this many sectors, which bounds both the DMA work
// and the PIO staging buffer of a single request.
constexpr uint32_t kMaxSectors = 40; // 20 KiB
constexpr uint32_t kMaxTransfer = kMaxSectors * kSectorSize;
// Inspection window of the tool-only provider, in sectors.
constexpr uint32_t kPreviewSectors = 32;

// Register block.  LBA, COUNT and DMA_ADDRESS are 32-bit registers that occupy
// four consecutive byte addresses; a bus master that cannot write 32 bits at
// once builds them one byte at a time.
constexpr uint32_t kRegisterBase = 0x00;
constexpr uint32_t kRegisterStatus = 0x00;
constexpr uint32_t kRegisterCommand = 0x04;
constexpr uint32_t kRegisterLba = 0x08;
constexpr uint32_t kRegisterCount = 0x0C;
constexpr uint32_t kRegisterDma = 0x10;
constexpr uint32_t kRegisterError = 0x14;
constexpr uint32_t kRegisterMediaId = 0x18;
constexpr uint32_t kRegisterControl = 0x1C;
constexpr uint32_t kRegisterData = 0x20;
constexpr uint32_t kRegisterEnd = 0x24;

enum {
    kStatusReady = 1u << 0,
    kStatusBusy = 1u << 1,
    kStatusError = 1u << 2,
    kStatusWriteProtected = 1u << 3,
    kStatusMediaPresent = 1u << 4,
    kStatusMediaChanged = 1u << 5,
    kStatusIrqPending = 1u << 6,
    kStatusDmaSupported = 1u << 7
};

enum {
    kErrorNone = 0,
    kErrorNoMedia = 1,
    kErrorInvalidLba = 2,
    kErrorWriteProtected = 3,
    kErrorMediaChanged = 4,
    kErrorDmaFault = 5,
    kErrorHostIo = 6,
    kErrorInvalidCommand = 7,
    kErrorOverflow = 8
};

const char *const kErrorNames[] = {"NONE",            "NO_MEDIA",        "INVALID_LBA",
                                   "WRITE_PROTECTED", "MEDIA_CHANGED",   "DMA_FAULT",
                                   "HOST_IO_ERROR",   "INVALID_COMMAND", "TRANSFER_TOO_LARGE"};

enum {
    kCommandNop = 0x00,
    kCommandRead = 0x01,
    kCommandWrite = 0x02,
    kCommandFlush = 0x03,
    kCommandIdentify = 0x04,
    kCommandReset = 0x05
};

// IDENTIFY reports the mounted medium type in DATA.  0x00014446 is "FD\x01".
constexpr uint32_t kIdentifyMedium = 0x00014446;

constexpr uint32_t kControlDmaEnable = 1u << 0;
constexpr uint32_t kControlIrqEnable = 1u << 1;
constexpr uint32_t kControlAckIrq = 1u << 6;
constexpr uint32_t kControlAckError = 1u << 7;

constexpr const char *kProtocol = "srz80.floppy.v1";

uint32_t load_word(const uint8_t *bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
           (uint32_t(bytes[3]) << 24);
}

void store_word(uint8_t *bytes, uint32_t word) {
    bytes[0] = uint8_t(word & 0xFF);
    bytes[1] = uint8_t((word >> 8) & 0xFF);
    bytes[2] = uint8_t((word >> 16) & 0xFF);
    bytes[3] = uint8_t((word >> 24) & 0xFF);
}

// Opens the backing image for update.  The C handle is the portable way to
// reach the descriptor a durability flush needs, and keeping it alive for the
// whole operation avoids reopening the file between read and write.
std::FILE *open_backing(const std::string &path, const char *mode) {
#if defined(_WIN32)
    std::FILE *file = nullptr;
    if (fopen_s(&file, path.c_str(), mode) != 0)
        return nullptr;
    return file;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// Makes an already flushed file durable.  FLUSH is a promise about the backing
// image, so it pushes past the host's own cache as well.
bool sync_backing(std::FILE *file) {
    if (!file)
        return false;
    if (std::fflush(file) != 0)
        return false;
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

// Removable medium abstraction: an in-memory 1.44 MB medium plus the host file
// it is backed by.  The guest-visible behaviour above is stated in medium terms
// only; this class is where a host file is allowed to exist.
class Medium {
  public:
    bool present() const { return present_; }
    bool write_protected() const { return write_protected_; }
    bool write_error() const { return write_error_; }
    const std::string &image() const { return image_; }
    const uint8_t *sector(uint32_t lba) const { return cache_.data() + size_t(lba) * kSectorSize; }

    bool insert(const std::string &path) {
        std::FILE *probe = open_backing(path, "rb");
        if (!probe)
            return false;
        if (std::fseek(probe, 0, SEEK_END) != 0) {
            std::fclose(probe);
            return false;
        }
        const long size = std::ftell(probe);
        if (size != long(kCapacity)) {
            std::fclose(probe);
            return false;
        }
        std::rewind(probe);
        std::vector<uint8_t> loaded(kCapacity);
        const size_t read = std::fread(loaded.data(), 1, kCapacity, probe);
        std::fclose(probe);
        if (read != kCapacity)
            return false;
        // A medium the emulator cannot open for update is write protected.  The
        // guest learns that from STATUS and never from a host error.
        std::FILE *writable = open_backing(path, "r+b");
        write_protected_ = writable == nullptr;
        if (writable)
            std::fclose(writable);
        cache_ = std::move(loaded);
        dirty_.assign(kSectorCount, 0);
        image_ = path;
        present_ = true;
        write_error_ = false;
        return true;
    }

    // Creation is a card operation, just like insertion.  The tool chooses a
    // path through a host-owned save dialog but never writes the medium itself.
    // Refuse an existing path: the action must not silently destroy a disk the
    // user selected by mistake.
    bool create_blank(const std::string &path) {
        if (std::FILE *existing = open_backing(path, "rb")) {
            std::fclose(existing);
            return false;
        }
        std::FILE *file = open_backing(path, "wb");
        if (!file)
            return false;
        std::array<uint8_t, 32 * 1024> zeros{};
        uint64_t remaining = kCapacity;
        bool written = true;
        while (remaining) {
            const size_t amount = size_t(std::min<uint64_t>(remaining, zeros.size()));
            if (std::fwrite(zeros.data(), 1, amount, file) != amount) {
                written = false;
                break;
            }
            remaining -= amount;
        }
        if (written)
            written = sync_backing(file);
        std::fclose(file);
        return written && insert(path);
    }

    void eject() {
        cache_.clear();
        cache_.shrink_to_fit();
        dirty_.clear();
        dirty_.shrink_to_fit();
        image_.clear();
        present_ = false;
        write_protected_ = false;
        write_error_ = false;
    }

    void read(uint32_t lba, uint8_t *out) const {
        std::memcpy(out, sector(lba), kSectorSize);
    }

    // Whole commands are committed at once: either every requested sector
    // reaches the medium or none of them does.
    bool write(uint32_t lba, uint32_t count, const uint8_t *data) {
        if (write_protected_)
            return false;
        if (!store(lba, count, data))
            return false;
        std::memcpy(cache_.data() + size_t(lba) * kSectorSize, data, size_t(count) * kSectorSize);
        for (uint32_t index = 0; index < count; ++index)
            dirty_[lba + index] = 1;
        return true;
    }

    // Commits every stale sector, then makes the backing image durable when
    // `durable` is set.  False means the medium does not hold the writes.
    bool flush(bool durable) {
        if (!present_)
            return true;
        std::vector<uint32_t> stale;
        for (uint32_t lba = 0; lba < kSectorCount; ++lba)
            if (dirty_[lba])
                stale.push_back(lba);
        if (stale.empty())
            return !durable || sync_medium();
        if (write_protected_)
            return false;
        std::FILE *file = open_backing(image_, "r+b");
        if (!file) {
            write_error_ = true;
            return false;
        }
        bool stored = true;
        if (stale.size() > kSectorCount / 2) {
            stored = std::fseek(file, 0, SEEK_SET) == 0 &&
                     std::fwrite(cache_.data(), 1, kCapacity, file) == kCapacity;
        } else {
            for (const uint32_t lba : stale) {
                if (std::fseek(file, long(uint64_t(lba) * kSectorSize), SEEK_SET) != 0 ||
                    std::fwrite(sector(lba), 1, kSectorSize, file) != kSectorSize)
                    stored = false;
            }
        }
        if (stored && durable)
            stored = sync_backing(file);
        else if (stored)
            stored = std::fflush(file) == 0;
        std::fclose(file);
        if (!stored) {
            write_error_ = true;
            return false;
        }
        for (const uint32_t lba : stale)
            dirty_[lba] = 0;
        return true;
    }

    // Reads the whole backing image back and compares it with the medium.  The
    // tool exposes this as a review action; it is not part of a command.
    bool verify() {
        if (!present_)
            return true;
        std::FILE *file = open_backing(image_, "rb");
        if (!file)
            return false;
        std::array<uint8_t, kSectorSize> sample{};
        bool matches = true;
        for (uint32_t lba = 0; lba < kSectorCount && matches; ++lba) {
            matches = std::fseek(file, long(uint64_t(lba) * kSectorSize), SEEK_SET) == 0 &&
                      std::fread(sample.data(), 1, kSectorSize, file) == kSectorSize &&
                      std::memcmp(sample.data(), sector(lba), kSectorSize) == 0;
        }
        std::fclose(file);
        return matches;
    }

  private:
    bool sync_medium() {
        std::FILE *file = open_backing(image_, "r+b");
        if (!file) {
            write_error_ = true;
            return false;
        }
        const bool ok = sync_backing(file);
        std::fclose(file);
        if (!ok)
            write_error_ = true;
        return ok;
    }

    // Writes one command's sectors and confirms the bytes came back, which is
    // how a write-protected or full host filesystem is caught before the card
    // reports a successful write.
    bool store(uint32_t lba, uint32_t count, const uint8_t *data) {
        std::FILE *file = open_backing(image_, "r+b");
        if (!file) {
            write_error_ = true;
            return false;
        }
        const bool wrote = std::fseek(file, long(uint64_t(lba) * kSectorSize), SEEK_SET) == 0 &&
                           std::fwrite(data, 1, size_t(count) * kSectorSize, file) ==
                               size_t(count) * kSectorSize &&
                           std::fflush(file) == 0;
        std::fclose(file);
        if (!wrote) {
            write_error_ = true;
            return false;
        }
        std::FILE *check = open_backing(image_, "rb");
        if (!check)
            return false;
        std::array<uint8_t, kSectorSize> sample{};
        bool matches = true;
        for (uint32_t index = 0; index < count && matches; ++index) {
            matches = std::fseek(check, long(uint64_t(lba + index) * kSectorSize), SEEK_SET) == 0 &&
                      std::fread(sample.data(), 1, kSectorSize, check) == kSectorSize &&
                      std::memcmp(sample.data(), data + size_t(index) * kSectorSize, kSectorSize) == 0;
        }
        std::fclose(check);
        if (!matches)
            write_error_ = true;
        return matches;
    }

    bool present_ = false, write_protected_ = false, write_error_ = false;
    std::string image_;
    std::vector<uint8_t> cache_;
    std::vector<uint8_t> dirty_;
};

struct Floppy {
    const ShouryoHost *host = nullptr;
    const SrhHostProvidersV1 *providers = nullptr;
    const SrhHostSignalsV1 *signals = nullptr;
    SrhHandle owner = 0, irq = 0;
    uint64_t base = 0;
    SrhHandle dma_space = 0;

    std::array<uint8_t, kRegisterEnd> regs{};
    uint32_t command_bytes = 0;
    uint32_t sequence = 0, completed = 0;

    Medium medium;
    std::array<uint8_t, kMaxTransfer> stage{};
    uint32_t staged = 0, drained = 0;
    bool staged_payload = false;
    // DATA is a 32-bit word register over the staged payload.  A master that can
    // only move bytes still sees whole words: byte 0 is the low byte of one
    // transfer and the remaining bytes are the higher lanes of that same word.
    uint32_t data_word = 0, data_write_cursor = 0;

    bool busy = false, media_changed = false, irq_pending = false, irq_driven = false;

    std::string command_error;
    uint32_t inspect_lba = 0, last_lba = 0, last_count = 0;
    bool flush_after_write = false;
    // Host-side activity indicator, in simulation time.  The guest has no
    // register for it: it exists so the tool's access lamp can blink.
    uint64_t access_ns = 0, access_count = 0;

    uint64_t simulation_time_ns() const {
        uint64_t now = 0;
        if (providers && providers->simulation_time_ns(providers->context, &now) == SRH_OK)
            return now;
        return 0;
    }

    void record_access() {
        access_ns = simulation_time_ns();
        ++access_count;
    }

    uint32_t lba() const { return load_word(&regs[kRegisterLba]); }
    uint32_t count() const { return load_word(&regs[kRegisterCount]); }
    uint32_t dma_address() const { return load_word(&regs[kRegisterDma]); }
    uint32_t error() const { return load_word(&regs[kRegisterError]); }
    uint32_t media_id() const { return load_word(&regs[kRegisterMediaId]); }
    uint32_t control() const { return load_word(&regs[kRegisterControl]); }
    void set_error(uint32_t code) { store_word(&regs[kRegisterError], code); }
    void set_lba(uint32_t value) { store_word(&regs[kRegisterLba], value); }
    void set_count(uint32_t value) { store_word(&regs[kRegisterCount], value); }
    void set_dma_address(uint32_t value) { store_word(&regs[kRegisterDma], value); }
    void set_media_id(uint32_t value) { store_word(&regs[kRegisterMediaId], value); }

    bool dma_enabled() const { return (control() & kControlDmaEnable) != 0; }
    bool irq_enabled() const { return (control() & kControlIrqEnable) != 0; }
    bool busy_register() const { return (regs[kRegisterStatus] & kStatusBusy) != 0; }

    // STATUS is rebuilt from the live card state every time it changes, so a
    // guest that only ever polls STATUS never sees a stale bit.
    void set_busy(bool value) {
        busy = value;
        refresh_status();
    }

    void refresh_status() {
        uint8_t bits = kStatusDmaSupported;
        // READY says the drive itself can be addressed.  Whether a request can
        // succeed is stated by MEDIA_PRESENT, WRITE_PROTECTED and ERROR.
        if (!busy)
            bits |= kStatusReady;
        else
            bits |= kStatusBusy;
        if (medium.present())
            bits |= kStatusMediaPresent;
        if (medium.write_protected())
            bits |= kStatusWriteProtected;
        if (error() != kErrorNone || medium.write_error())
            bits |= kStatusError;
        if (media_changed)
            bits |= kStatusMediaChanged;
        if (irq_pending)
            bits |= kStatusIrqPending;
        regs[kRegisterStatus] = bits;
    }

    static const char *error_name(uint32_t code) {
        constexpr uint32_t known = sizeof(kErrorNames) / sizeof(kErrorNames[0]);
        return code < known ? kErrorNames[code] : "UNKNOWN";
    }

    // The physical interrupt line follows the pending bit: raised when a
    // completion needs attention, released when the guest acknowledges it.
    void update_irq() {
        if (!irq || !signals)
            return;
        if (irq_pending && !irq_driven) {
            if (host->signal_drive(host->context, owner, irq, 1, 0) == SRH_OK)
                irq_driven = true;
        } else if (!irq_pending && irq_driven) {
            signals->release(signals->context, owner, irq);
            irq_driven = false;
        }
    }

    // Drops the transfer bookkeeping.  The DATA word register is deliberately
    // untouched: the guest may still be reading the word that was just handed
    // to it when the buffer empties.
    void discard_stage() {
        staged = 0;
        drained = 0;
        staged_payload = false;
        data_write_cursor = 0;
    }

    // Settles one executed command and publishes the observable result.  A
    // command whose payload the guest has not consumed yet keeps its staging
    // buffer, so DATA still holds the result after the command completes.
    void finish(uint32_t result) {
        if (!staged_payload)
            discard_stage();
        set_error(result);
        command_error = result == kErrorNone ? std::string() : error_name(result);
        if (irq_enabled())
            irq_pending = true;
        completed = sequence;
        refresh_status();
        update_irq();
    }

    // Optional write-through: a completed WRITE normally means the medium in
    // emulator memory is updated and the host image follows on FLUSH or eject.
    // With write-through enabled every success also reaches the host file, at
    // the cost of host I/O inside the command.
    void flush_after_command(uint32_t opcode, uint32_t result) {
        if (!flush_after_write || result != kErrorNone || opcode != kCommandWrite)
            return;
        if (medium.present() && !medium.flush(false)) {
            set_error(medium.write_protected() ? kErrorWriteProtected : kErrorHostIo);
            command_error = "the backing image could not be updated";
        }
        refresh_status();
    }

    // Every insertion and every ejection is a first-class hardware event.  The
    // medium generation advances, an operation that started on the previous
    // generation is invalidated instead of finishing, and the guest is
    // interrupted when it asked to be.
    void media_event() {
        set_media_id(media_id() + 1);
        media_changed = true;
        if (busy) {
            busy = false;
            discard_stage();
            set_error(kErrorMediaChanged);
            command_error = error_name(kErrorMediaChanged);
        }
        if (irq_enabled())
            irq_pending = true;
        refresh_status();
        update_irq();
    }

    bool media_still_current(uint32_t generation) {
        if (media_id() == generation)
            return true;
        busy = false;
        discard_stage();
        set_error(kErrorMediaChanged);
        command_error = error_name(kErrorMediaChanged);
        refresh_status();
        return false;
    }

    // The request is validated before any transfer starts, and every transfer
    // is confined to the space this card was created in, so floppy DMA can
    // only land in RAM.
    uint32_t validate(uint32_t *bytes) const {
        if (!medium.present())
            return kErrorNoMedia;
        const uint32_t sectors = count();
        if (sectors == 0 || sectors > kMaxSectors)
            return sectors > kMaxSectors ? kErrorOverflow : kErrorInvalidLba;
        const uint32_t at = lba();
        if (at >= kSectorCount || sectors > kSectorCount - at)
            return kErrorInvalidLba;
        *bytes = sectors * kSectorSize;
        return kErrorNone;
    }

    bool dma_run_ok(uint32_t bytes) const {
        return uint64_t(dma_address()) + bytes <= uint64_t(UINT32_MAX);
    }

    uint32_t read_guest(uint8_t *out, uint32_t bytes) {
        for (uint32_t index = 0; index < bytes; ++index) {
            uint8_t byte = 0;
            if (host->read(host->context, owner, dma_space, uint64_t(dma_address()) + index, &byte) !=
                SRH_OK)
                return kErrorDmaFault;
            out[index] = byte;
        }
        return kErrorNone;
    }

    uint32_t write_guest(const uint8_t *data, uint32_t bytes) {
        for (uint32_t index = 0; index < bytes; ++index) {
            if (host->write(host->context, owner, dma_space, uint64_t(dma_address()) + index,
                            data[index]) != SRH_OK)
                return kErrorDmaFault;
        }
        return kErrorNone;
    }

    uint32_t execute(uint32_t opcode) {
        switch (opcode) {
        case kCommandNop:
            return kErrorNone;
        case kCommandFlush:
            return execute_flush();
        case kCommandIdentify:
            return execute_identify();
        case kCommandReset:
            return execute_reset();
        case kCommandRead:
            return execute_read();
        case kCommandWrite:
            return execute_write();
        default:
            return kErrorInvalidCommand;
        }
    }

    uint32_t execute_flush() {
        if (!medium.present())
            return kErrorNoMedia;
        if (!medium.flush(true))
            return medium.write_protected() ? kErrorWriteProtected : kErrorHostIo;
        return kErrorNone;
    }

    uint32_t execute_identify() {
        if (!medium.present())
            return kErrorNoMedia;
        store_word(stage.data(), kIdentifyMedium);
        staged = 4;
        drained = 0;
        staged_payload = true;
        return kErrorNone;
    }

    uint32_t execute_reset() {
        set_lba(0);
        set_count(0);
        set_dma_address(0);
        store_word(&regs[kRegisterControl], 0);
        media_changed = false;
        irq_pending = false;
        discard_stage();
        update_irq();
        return kErrorNone;
    }

    uint32_t execute_read() {
        uint32_t bytes = 0;
        const uint32_t rejected = validate(&bytes);
        if (rejected != kErrorNone)
            return rejected;
        const uint32_t at = lba(), sectors = count();
        for (uint32_t sector = 0; sector < sectors; ++sector)
            medium.read(at + sector, stage.data() + size_t(sector) * kSectorSize);
        last_lba = at;
        last_count = sectors;
        staged = bytes;
        drained = 0;
        if (!dma_enabled()) {
            // The guest clocks the payload through DATA, so it must survive
            // command completion.
            staged_payload = true;
            return kErrorNone;
        }
        if (!dma_run_ok(bytes))
            return kErrorDmaFault;
        const uint32_t generation = media_id();
        set_busy(true);
        const uint32_t result = write_guest(stage.data(), bytes);
        busy = false;
        if (!media_still_current(generation))
            return kErrorMediaChanged;
        refresh_status();
        if (result != kErrorNone)
            return result;
        discard_stage();
        return kErrorNone;
    }

    uint32_t execute_write() {
        uint32_t bytes = 0;
        const uint32_t rejected = validate(&bytes);
        if (rejected != kErrorNone)
            return rejected;
        if (medium.write_protected())
            return kErrorWriteProtected;
        last_lba = lba();
        last_count = count();
        if (!dma_enabled())
            return kErrorNone; // The guest clocks the payload through DATA.
        if (!dma_run_ok(bytes))
            return kErrorDmaFault;
        const uint32_t generation = media_id();
        set_busy(true);
        const uint32_t read_result = read_guest(stage.data(), bytes);
        busy = false;
        if (!media_still_current(generation))
            return kErrorMediaChanged;
        refresh_status();
        if (read_result != kErrorNone)
            return read_result;
        const uint32_t result = commit();
        discard_stage();
        return result;
    }

    uint32_t commit() {
        if (!medium.present())
            return kErrorNoMedia;
        if (!medium.write(lba(), count(), stage.data()))
            return medium.write_protected() ? kErrorWriteProtected : kErrorHostIo;
        return kErrorNone;
    }

    // The complete 32-bit opcode is visible only once all four bytes of COMMAND
    // have arrived, so a byte-at-a-time master executes exactly one command.
    void write_command_byte(uint32_t value) {
        regs[kRegisterCommand + command_bytes] = uint8_t(value & 0xFF);
        ++command_bytes;
        if (command_bytes < 4)
            return;
        command_bytes = 0;
        ++sequence;
        const uint32_t opcode = load_word(&regs[kRegisterCommand]);
        if (busy_register()) {
            set_error(kErrorOverflow);
            command_error = "command rejected while the card is busy";
            refresh_status();
            return;
        }
        const uint32_t result = execute(opcode);
        finish(result);
        flush_after_command(opcode, result);
    }

    // CONTROL is a 32-bit register.  A master that can only write bytes sets the
    // enables with a write to CONTROL+0 and acknowledges with CONTROL+3; a write
    // to a byte that carries neither must not clear anything.
    void write_control(uint32_t value, bool full) {
        if (full)
            store_word(&regs[kRegisterControl], value & (kControlDmaEnable | kControlIrqEnable));
        const uint32_t acknowledge = value & (kControlAckError | kControlAckIrq);
        if (acknowledge & kControlAckError) {
            set_error(kErrorNone);
            command_error.clear();
        }
        if (acknowledge) {
            irq_pending = false;
            // The acknowledge point is also how the guest consumes a medium
            // change; MEDIA_ID remains the authority on how many happened.
            media_changed = false;
        }
        refresh_status();
        update_irq();
    }

    // DATA is the PIO path.  A complete request is committed at once, so a
    // partially clocked write never reaches the medium, and a master that moves
    // one byte lane at a time still delivers whole words.
    void write_data(uint32_t lane, uint32_t value) {
        if (busy) {
            set_error(kErrorOverflow);
            refresh_status();
            return;
        }
        if (error() != kErrorNone)
            return;
        if (!medium.present()) {
            set_error(kErrorNoMedia);
            refresh_status();
            return;
        }
        if (medium.write_protected()) {
            set_error(kErrorWriteProtected);
            refresh_status();
            return;
        }
        const uint32_t expected = count() * kSectorSize;
        if (expected == 0 || expected > kMaxTransfer) {
            set_error(expected == 0 ? kErrorInvalidCommand : kErrorOverflow);
            refresh_status();
            return;
        }
        if (staged + 4 > expected) {
            set_error(kErrorOverflow);
            refresh_status();
            return;
        }
        if (lane == 0) {
            data_word = value;
            data_write_cursor = 1;
        } else {
            if (lane != data_write_cursor) {
                set_error(kErrorInvalidCommand);
                refresh_status();
                return;
            }
            data_word |= (value & 0xFF) << (8 * lane);
            data_write_cursor = lane + 1;
        }
        if (data_write_cursor < 4)
            return;
        data_write_cursor = 0;
        store_word(stage.data() + staged, data_word);
        staged += 4;
        if (staged != expected)
            return;
        const uint32_t generation = media_id();
        uint32_t result = commit();
        if (result == kErrorNone && media_id() != generation)
            result = kErrorMediaChanged;
        finish(result);
        flush_after_command(kCommandWrite, result);
    }

    SrhStatus read(uint64_t address, uint8_t *value, bool peek) {
        if (!value || address < base || address - base >= kRegisterEnd)
            return SRH_INVALID;
        const auto offset = uint32_t(address - base);
        if (offset >= kRegisterData && offset < kRegisterData + 4) {
            if (peek) {
                *value = (offset - kRegisterData == 0 && drained < staged) ? stage[drained] : 0;
                return SRH_OK;
            }
            if (offset == kRegisterData) {
                data_word = 0;
                if (drained + 4 <= staged) {
                    data_word = load_word(&stage[drained]);
                    drained += 4;
                    if (drained == staged)
                        discard_stage();
                }
            }
            *value = uint8_t((data_word >> (8 * (offset - kRegisterData))) & 0xFF);
            record_access();
            return SRH_OK;
        }
        *value = regs[offset];
        if (!peek)
            record_access();
        return SRH_OK;
    }

    SrhStatus write(uint64_t address, uint8_t value) {
        if (address < base || address - base >= kRegisterEnd)
            return SRH_INVALID;
        record_access();
        const auto offset = uint32_t(address - base);
        switch (offset & ~0x3u) {
        case kRegisterCommand:
            write_command_byte(value);
            break;
        case kRegisterLba:
        case kRegisterCount:
        case kRegisterDma:
            regs[offset] = value;
            break;
        case kRegisterError:
            regs[offset] = value;
            command_error.clear();
            refresh_status();
            break;
        case kRegisterMediaId:
            break; // read-only
        case kRegisterControl:
            write_control(value << (8 * (offset - kRegisterControl)),
                          offset == kRegisterControl);
            break;
        case kRegisterData:
            write_data(offset - kRegisterData, value);
            break;
        default:
            break;
        }
        return SRH_OK;
    }
};

// ---------------------------------------------------------------------------
// Emulator-only boundary: the provider a tool uses to act like a human
// inserting and removing disks.  Nothing below this line is visible to the
// simulated machine, which learns about media only through STATUS/MEDIA_ID.
// ---------------------------------------------------------------------------

std::string hex_bytes(const uint8_t *bytes, uint32_t count) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(size_t(count) * 2);
    for (uint32_t index = 0; index < count; ++index) {
        text.push_back(digits[bytes[index] >> 4]);
        text.push_back(digits[bytes[index] & 0x0F]);
    }
    return text;
}

int hex_digit(char character) {
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

SrhStatus SRH_CALL read(void *p, uint64_t address, uint8_t *value);
SrhStatus SRH_CALL write(void *p, uint64_t address, uint8_t value);
SrhStatus SRH_CALL peek(void *p, uint64_t address, uint8_t *value);

Json snapshot_json(const Floppy &floppy) {
    Json out{{"schema", 1},
             {"medium", "f1440"},
             {"media_present", floppy.medium.present()},
             {"media_id", floppy.media_id()},
             {"media_changed", floppy.media_changed},
             {"sectors", kSectorCount},
             {"sector_size", kSectorSize},
             {"capacity", kCapacity},
             {"write_protected", floppy.medium.write_protected()},
             {"host_write_error", floppy.medium.write_error()},
             {"busy", floppy.busy},
             {"irq_pending", floppy.irq_pending},
             {"error", floppy.error()},
             {"error_name", Floppy::error_name(floppy.error())},
             {"sequence", floppy.sequence},
             {"completed", floppy.completed},
             {"lba", floppy.lba()},
             {"count", floppy.count()},
             {"dma_address", floppy.dma_address()},
             {"dma_enabled", floppy.dma_enabled()},
             {"irq_enabled", floppy.irq_enabled()},
             {"last_lba", floppy.last_lba},
             {"last_count", floppy.last_count},
             {"image", floppy.medium.image()},
             {"status", SrhStatus(SRH_OK)},
             {"command_error", floppy.command_error},
             {"access_ns", floppy.access_ns},
             {"access_count", floppy.access_count},
             {"sim_time_ns", floppy.simulation_time_ns()},
             {"inspect_lba", floppy.inspect_lba},
             {"inspect", ""},
             {"inspect_sectors", 0}};
    if (floppy.medium.present()) {
        const uint32_t first = floppy.inspect_lba;
        const uint32_t sectors = std::min(kPreviewSectors, kSectorCount - first);
        out["inspect_sectors"] = sectors;
        out["inspect"] = hex_bytes(floppy.medium.sector(first), kSectorSize);
    }
    return out;
}

SrhStatus SRH_CALL snapshot(void *p, char *buffer, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!size)
            return SRH_INVALID;
        auto &floppy = *static_cast<Floppy *>(p);
        const std::string text = snapshot_json(floppy).dump();
        const uint64_t required = text.size() + 1;
        const uint64_t capacity = *size;
        *size = required;
        if (!buffer)
            return SRH_OK;
        if (capacity < required)
            return SRH_INVALID;
        std::memcpy(buffer, text.c_str(), static_cast<size_t>(required));
        return SRH_OK;
    });
}

// The tool asks for one medium operation at a time.  A rejected request never
// changes card state, and the reason travels back in "command_error".
SrhStatus SRH_CALL command(void *p, uint32_t kind, uint64_t, const char *text, uint64_t size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!text || !size || size >= SRH_PROVIDER_MAX_BYTES || kind > SRH_PROVIDER_CONFIGURE)
            return SRH_INVALID;
        auto &floppy = *static_cast<Floppy *>(p);
        const auto request = Json::parse(text, text + size, nullptr, false);
        if (request.is_discarded() || !request.is_object())
            return SRH_INVALID;
        const auto operation = request.value("op", std::string());
        const uint64_t generation = floppy.media_id();
        if (operation == "insert") {
            const auto image = request.value("image", std::string());
            if (image.empty()) {
                floppy.command_error = "no image path supplied";
                return SRH_INVALID;
            }
            if (!floppy.medium.insert(image)) {
                floppy.command_error = "not a 1.44 MB raw image: " + image;
                return SRH_UNAVAILABLE;
            }
            floppy.media_event();
            floppy.command_error.clear();
            return SRH_OK;
        }
        if (operation == "create") {
            const auto image = request.value("image", std::string());
            if (image.empty()) {
                floppy.command_error = "no image path supplied";
                return SRH_INVALID;
            }
            if (!floppy.medium.create_blank(image)) {
                floppy.command_error = "could not create a new blank 1.44 MB image (the path may already exist)";
                return SRH_UNAVAILABLE;
            }
            floppy.media_event();
            floppy.command_error.clear();
            return SRH_OK;
        }
        if (operation == "eject") {
            if (floppy.medium.present()) {
                // Eject implicitly performs FLUSH, and it must not outlive the
                // media generation it belonged to.
                const bool flushed = floppy.medium.flush(true);
                const bool lost = !flushed && !floppy.medium.write_protected();
                floppy.medium.eject();
                floppy.media_event();
                if (lost) {
                    floppy.set_error(kErrorHostIo);
                    floppy.command_error = "the backing image could not be updated";
                    floppy.refresh_status();
                    return SRH_ERROR;
                }
            } else {
                // Ejecting an empty drive is still a media event: the drive
                // changes generation so software can trust MEDIA_ID.
                floppy.media_event();
            }
            floppy.command_error.clear();
            return SRH_OK;
        }
        if (operation == "flush") {
            if (!floppy.medium.present()) {
                floppy.command_error = "no medium is inserted";
                return SRH_UNAVAILABLE;
            }
            if (!floppy.medium.flush(true)) {
                floppy.set_error(floppy.medium.write_protected() ? kErrorWriteProtected : kErrorHostIo);
                floppy.command_error = "the backing image could not be updated";
                floppy.refresh_status();
                return SRH_ERROR;
            }
            floppy.command_error.clear();
            return SRH_OK;
        }
        if (operation == "verify") {
            if (!floppy.medium.present()) {
                floppy.command_error = "no medium is inserted";
                return SRH_UNAVAILABLE;
            }
            if (!floppy.medium.verify()) {
                floppy.command_error = "the backing image does not match the medium";
                return SRH_ERROR;
            }
            floppy.command_error.clear();
            return SRH_OK;
        }
        if (operation == "configure") {
            if (request.contains("flush_after_write")) {
                if (!request["flush_after_write"].is_boolean())
                    return SRH_INVALID;
                floppy.flush_after_write = request["flush_after_write"].get<bool>();
            }
            floppy.command_error.clear();
            return SRH_OK;
        }
        if (operation == "seek") {
            const auto target = request.value("lba", uint64_t{0});
            if (target >= kSectorCount) {
                floppy.command_error = "sector is outside the medium";
                return SRH_INVALID;
            }
            floppy.inspect_lba = uint32_t(target);
            floppy.command_error.clear();
            return SRH_OK;
        }
        if (operation == "write_sector") {
            if (!floppy.medium.present()) {
                floppy.command_error = "no medium is inserted";
                return SRH_UNAVAILABLE;
            }
            if (floppy.medium.write_protected()) {
                floppy.command_error = "the medium is write protected";
                return SRH_CONFLICT;
            }
            const auto encoded = request.value("hex", std::string());
            const auto target = request.value("lba", uint64_t(floppy.inspect_lba));
            if (target >= kSectorCount) {
                floppy.command_error = "sector is outside the medium";
                return SRH_INVALID;
            }
            if (encoded.size() != size_t(kSectorSize) * 2) {
                floppy.command_error = "a sector patch needs exactly 1024 hex digits";
                return SRH_INVALID;
            }
            std::array<uint8_t, kSectorSize> bytes{};
            for (uint32_t index = 0; index < kSectorSize; ++index) {
                const auto high = hex_digit(encoded[size_t(index) * 2]);
                const auto low = hex_digit(encoded[size_t(index) * 2 + 1]);
                if (high < 0 || low < 0) {
                    floppy.command_error = "sector patch is not valid hex";
                    return SRH_INVALID;
                }
                bytes[index] = uint8_t((high << 4) | low);
            }
            if (!floppy.medium.write(uint32_t(target), 1, bytes.data())) {
                floppy.command_error = "the medium could not be updated";
                return SRH_ERROR;
            }
            floppy.inspect_lba = uint32_t((target + 1) % kSectorCount);
            floppy.command_error.clear();
            return SRH_OK;
        }
        floppy.command_error = "unknown media operation: " + operation;
        (void)generation;
        return SRH_INVALID;
    });
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner, const SrhConfig *config, void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !out || !config->space ||
            !srz80::sdk::has_field(host, &ShouryoHost::query) || !host->query ||
            config->base > UINT64_MAX - kRegisterEnd)
            return SRH_INVALID;
        const void *extension = nullptr;
        if (host->query(host->context, "host.providers.v1", &extension) != SRH_OK)
            return SRH_UNAVAILABLE;
        const auto *providers = static_cast<const SrhHostProvidersV1 *>(extension);
        if (!srz80::sdk::valid(providers) || !providers->register_provider ||
            !providers->simulation_time_ns)
            return SRH_INVALID;
        auto floppy = std::make_unique<Floppy>();
        floppy->host = host;
        floppy->providers = providers;
        floppy->owner = owner;
        floppy->base = config->base;
        floppy->dma_space = config->space;
        auto config_json = config->config_json
                               ? Json::parse(config->config_json,
                                             config->config_json + config->config_json_size, nullptr,
                                             false)
                               : Json::object();
        if (config_json.is_discarded() || !config_json.is_object())
            return SRH_INVALID;
        if (config_json.contains("dma_space")) {
            if (!config_json["dma_space"].is_string())
                return SRH_INVALID;
            const auto name = config_json["dma_space"].get<std::string>();
            SrhHandle space = 0;
            if (!name.empty()) {
                const void *resources = nullptr;
                if (host->query(host->context, "host.resources.v1", &resources) != SRH_OK)
                    return SRH_UNAVAILABLE;
                const auto *lookup = static_cast<const SrhHostResourcesV1 *>(resources);
                if (!srz80::sdk::valid(lookup) || !lookup->lookup ||
                    lookup->lookup(lookup->context, "space", name.c_str(), &space) != SRH_OK)
                    return SRH_NOT_FOUND;
            }
            if (space)
                floppy->dma_space = space;
        }
        if (config_json.contains("irq")) {
            // An unwired interrupt line is a capability, not a failure: the
            // guest still observes IRQ_PENDING through STATUS.
            if (!config_json["irq"].is_string())
                return SRH_INVALID;
            const auto name = config_json["irq"].get<std::string>();
            if (!name.empty()) {
                if (host->query(host->context, "host.signals.v1", &extension) != SRH_OK)
                    return SRH_UNAVAILABLE;
                const auto *signals = static_cast<const SrhHostSignalsV1 *>(extension);
                if (!srz80::sdk::valid(signals) || !signals->release)
                    return SRH_INVALID;
                floppy->signals = signals;
                if (host->signal_find(host->context, name.c_str(), &floppy->irq) != SRH_OK)
                    return SRH_NOT_FOUND;
            }
        }
        // Write-through is a card-side policy: it changes when host I/O happens,
        // never what a completed WRITE means to the guest.
        if (config_json.contains("flush_after_write")) {
            if (!config_json["flush_after_write"].is_boolean())
                return SRH_INVALID;
            floppy->flush_after_write = config_json["flush_after_write"].get<bool>();
        }
        SrhMapping mapping{SRH_INIT(SrhMapping),
                           config->space,
                           config->base,
                           config->base + kRegisterEnd - 1,
                           config->priority,
                           floppy.get(),
                           read,
                           write,
                           peek,
                           nullptr};
        SrhHandle mapped = 0;
        if (host->map(host->context, owner, &mapping, &mapped) != SRH_OK)
            return SRH_ERROR;
        SrhDataProviderV1 provider{SRH_INIT(SrhDataProviderV1), floppy.get(), snapshot, command,
                                   "Floppy drive", {}, SRH_PROVIDER_LIVE_CONFIG};
        std::strcpy(provider.protocol, kProtocol);
        if (providers->register_provider(providers->context, owner, &provider) != SRH_OK)
            return SRH_ERROR;
        floppy->refresh_status();
        *out = floppy.release();
        return SRH_OK;
    });
}

SrhStatus SRH_CALL read(void *p, uint64_t address, uint8_t *value) {
    return srz80::sdk::guard([&] { return static_cast<Floppy *>(p)->read(address, value, false); });
}

SrhStatus SRH_CALL peek(void *p, uint64_t address, uint8_t *value) {
    return srz80::sdk::guard([&] { return static_cast<Floppy *>(p)->read(address, value, true); });
}

SrhStatus SRH_CALL write(void *p, uint64_t address, uint8_t value) {
    return srz80::sdk::guard([&] { return static_cast<Floppy *>(p)->write(address, value); });
}

void SRH_CALL destroy(void *p) {
    auto floppy = std::unique_ptr<Floppy>(static_cast<Floppy *>(p));
    if (!floppy)
        return;
    // A removed drive still owes the user a consistent backing image.
    if (floppy->medium.present()) {
        floppy->medium.flush(true);
        floppy->medium.eject();
    }
}

SrhStatus SRH_CALL reset(void *p, uint32_t) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        auto &floppy = *static_cast<Floppy *>(p);
        floppy.set_lba(0);
        floppy.set_count(0);
        floppy.set_dma_address(0);
        store_word(&floppy.regs[kRegisterControl], 0);
        floppy.set_error(kErrorNone);
        floppy.command_error.clear();
        floppy.discard_stage();
        floppy.command_bytes = 0;
        floppy.completed = floppy.sequence;
        floppy.media_changed = false;
        floppy.irq_pending = false;
        // The host may have already dropped this owner's drivers while resetting
        // the rack, so an acknowledge here is bookkeeping rather than a bus
        // operation that could fail.
        floppy.irq_driven = false;
        floppy.update_irq();
        floppy.refresh_status();
        return SRH_OK;
    });
}

SrhStatus SRH_CALL save_payload(void *p, uint8_t *buffer, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!size)
            return SRH_INVALID;
        auto &floppy = *static_cast<Floppy *>(p);
        const auto text = Json{{"regs", floppy.regs},
                               {"sequence", floppy.sequence},
                               {"completed", floppy.completed},
                               {"media_changed", floppy.media_changed},
                               {"irq_pending", floppy.irq_pending},
                               {"staged", floppy.stage},
                               {"staged_bytes", floppy.staged},
                               // The medium is host state: a state restore
                               // never inserts or removes a disk.
                               {"media_present", floppy.medium.present()}}
                              .dump();
        const uint64_t required = text.size();
        const uint64_t capacity = *size;
        *size = required;
        if (!buffer)
            return SRH_OK;
        if (capacity < required)
            return SRH_UNAVAILABLE;
        std::memcpy(buffer, text.data(), text.size());
        return SRH_OK;
    });
}

SrhStatus SRH_CALL load_payload(void *p, const uint8_t *buffer, uint64_t size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!p || !buffer || !size || size > 4 * 1024 * 1024)
            return SRH_INVALID;
        auto &floppy = *static_cast<Floppy *>(p);
        const auto state = Json::parse(buffer, buffer + size, nullptr, false);
        if (state.is_discarded() || !state.is_object() || !state.contains("regs") ||
            !state["regs"].is_array() || state["regs"].size() != floppy.regs.size() ||
            !state.contains("staged") || !state["staged"].is_array() ||
            state["staged"].size() != floppy.stage.size())
            return SRH_INVALID;
        std::array<uint8_t, kRegisterEnd> regs{};
        for (size_t index = 0; index < regs.size(); ++index) {
            if (!state["regs"][index].is_number_unsigned() || state["regs"][index].get<uint64_t>() > 255)
                return SRH_INVALID;
            regs[index] = state["regs"][index].get<uint8_t>();
        }
        std::array<uint8_t, kMaxTransfer> staged{};
        for (size_t index = 0; index < staged.size(); ++index) {
            if (!state["staged"][index].is_number_unsigned() || state["staged"][index].get<uint64_t>() > 255)
                return SRH_INVALID;
            staged[index] = state["staged"][index].get<uint8_t>();
        }
        for (const char *key : {"staged_bytes", "sequence", "completed"})
            if (!state.contains(key) || !state[key].is_number_unsigned()) return SRH_INVALID;
        for (const char *key : {"media_changed", "irq_pending"})
            if (!state.contains(key) || !state[key].is_boolean()) return SRH_INVALID;
        const auto staged_bytes = state.at("staged_bytes").get<uint64_t>();
        if (staged_bytes > kMaxTransfer)
            return SRH_INVALID;
        floppy.regs = regs;
        floppy.stage = staged;
        floppy.staged = uint32_t(staged_bytes);
        floppy.drained = 0;
        floppy.sequence = state.value("sequence", uint64_t(0));
        floppy.completed = state.value("completed", uint64_t(0));
        floppy.media_changed = state.value("media_changed", false);
        floppy.irq_pending = state.value("irq_pending", false);
        floppy.busy = false;
        floppy.command_bytes = 0;
        floppy.update_irq();
        floppy.refresh_status();
        return SRH_OK;
    });
}

SrhStatus SRH_CALL save_project_data(void *p, uint8_t *buffer, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!p || !size)
            return SRH_INVALID;
        auto &floppy = *static_cast<Floppy *>(p);
        const auto text = Json{{"schema", 1}, {"image", floppy.medium.image()}}.dump();
        const uint64_t required = text.size();
        const uint64_t capacity = *size;
        *size = required;
        if (!buffer)
            return SRH_OK;
        if (capacity < required)
            return SRH_UNAVAILABLE;
        std::memcpy(buffer, text.data(), text.size());
        return SRH_OK;
    });
}

SrhStatus SRH_CALL load_project_data(void *p, const uint8_t *buffer, uint64_t size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!p || !buffer || !size || size >= SRH_PROVIDER_MAX_BYTES)
            return SRH_INVALID;
        auto &floppy = *static_cast<Floppy *>(p);
        const auto state = Json::parse(buffer, buffer + size, nullptr, false);
        if (state.is_discarded() || !state.is_object() || state.value("schema", 0) != 1 ||
            !state.contains("image") || !state["image"].is_string())
            return SRH_INVALID;
        const auto image = state["image"].get<std::string>();
        if (image.empty())
            return SRH_OK;
        // A project whose medium is missing still loads: the drive is simply
        // empty, exactly as if the user had not inserted anything yet.
        if (!floppy.medium.insert(image)) {
            if (floppy.host && floppy.host->log) {
                const std::string message = "floppy: cannot insert " + image +
                                            " (expected exactly 1474560 bytes); drive left empty";
                floppy.host->log(floppy.host->context, floppy.owner, message.c_str());
            }
            return SRH_OK;
        }
        floppy.media_event();
        return SRH_OK;
    });
}

uint32_t SRH_CALL property_count(void *) { return 12; }

SrhStatus SRH_CALL property_info(void *, uint32_t index, SrhProperty *out) {
    if (!srz80::sdk::valid(out) || index >= property_count(nullptr))
        return SRH_INVALID;
    switch (index) {
    case 0:
        *out = {SRH_INIT(SrhProperty), "base", "Floppy", "Register block base address",
                SRH_UNSIGNED, 32, 16, 0, nullptr, SRH_PROPERTY_PERSISTENT};
        break;
    case 1:
        *out = {SRH_INIT(SrhProperty), "medium_type", "Floppy", "Mounted removable medium type",
                SRH_TEXT, 0, 0, 0, nullptr, 0};
        break;
    case 2:
        *out = {SRH_INIT(SrhProperty), "media_present", "Floppy", "MEDIA_PRESENT (STATUS bit 4)",
                SRH_BOOLEAN, 1, 10, 0, nullptr, 0};
        break;
    case 3:
        *out = {SRH_INIT(SrhProperty), "media_id", "Floppy",
                "MEDIA_ID, incremented by every insertion and ejection", SRH_UNSIGNED, 32, 10, 0,
                nullptr, 0};
        break;
    case 4:
        *out = {SRH_INIT(SrhProperty), "media_changed", "Floppy",
                "MEDIA_CHANGED (STATUS bit 5) is latched until the guest acknowledges it",
                SRH_BOOLEAN, 1, 10, 0, nullptr, 0};
        break;
    case 5:
        *out = {SRH_INIT(SrhProperty), "write_protected", "Floppy",
                "WRITE_PROTECTED (STATUS bit 3): the host image is read-only", SRH_BOOLEAN, 1, 10,
                0, nullptr, 0};
        break;
    case 6:
        *out = {SRH_INIT(SrhProperty), "busy", "Floppy", "BUSY (STATUS bit 1)", SRH_BOOLEAN, 1, 10,
                0, nullptr, 0};
        break;
    case 7:
        *out = {SRH_INIT(SrhProperty), "error", "Floppy",
                "ERROR (STATUS bit 2) and the sticky ERROR register code", SRH_UNSIGNED, 8, 10, 0,
                nullptr, 0};
        break;
    case 8:
        *out = {SRH_INIT(SrhProperty), "irq_pending", "Floppy", "IRQ_PENDING (STATUS bit 6)",
                SRH_BOOLEAN, 1, 10, 0, nullptr, 0};
        break;
    case 9:
        *out = {SRH_INIT(SrhProperty), "capacity", "Floppy",
                "Capacity of the removable medium in bytes", SRH_UNSIGNED, 32, 10, 0, nullptr, 0};
        break;
    case 10:
        *out = {SRH_INIT(SrhProperty), "last_request", "Floppy",
                "LBA and sector count of the most recent read or write", SRH_TEXT, 0, 0, 0, nullptr,
                0};
        break;
    default:
        *out = {SRH_INIT(SrhProperty), "media_image", "Floppy",
                "Host image backing the inserted medium (host-side device state, never visible to "
                "the simulated machine)",
                SRH_TEXT, 0, 0, 0, nullptr, SRH_PROPERTY_HIDE_UI | SRH_PROPERTY_RUNTIME};
        break;
    }
    return SRH_OK;
}

SrhStatus SRH_CALL property_get(void *p, uint32_t index, SrhValue *out) {
    if (!srz80::sdk::valid(out) || index >= property_count(nullptr))
        return SRH_INVALID;
    auto &floppy = *static_cast<Floppy *>(p);
    switch (index) {
    case 0:
        out->unsigned_value = floppy.base;
        break;
    case 1:
        std::snprintf(out->text, sizeof(out->text), "%s",
                      floppy.medium.present() ? "F1440 (1.44 MB, 2880 x 512)" : "none");
        break;
    case 2:
        out->unsigned_value = floppy.medium.present() ? 1 : 0;
        break;
    case 3:
        out->unsigned_value = floppy.media_id();
        break;
    case 4:
        out->unsigned_value = floppy.media_changed ? 1 : 0;
        break;
    case 5:
        out->unsigned_value = floppy.medium.write_protected() ? 1 : 0;
        break;
    case 6:
        out->unsigned_value = floppy.busy ? 1 : 0;
        break;
    case 7:
        out->unsigned_value = floppy.error();
        break;
    case 8:
        out->unsigned_value = floppy.irq_pending ? 1 : 0;
        break;
    case 9:
        out->unsigned_value = kCapacity;
        break;
    case 10:
        std::snprintf(out->text, sizeof(out->text), "LBA %u + %u", floppy.last_lba,
                      floppy.last_count);
        break;
    default:
        std::snprintf(out->text, sizeof(out->text), "%s", floppy.medium.image().c_str());
        break;
    }
    return SRH_OK;
}

SrhStatus SRH_CALL property_set(void *p, uint32_t index, const SrhValue *value) {
    if (!srz80::sdk::valid(value) || index != 0)
        return SRH_INVALID;
    auto &floppy = *static_cast<Floppy *>(p);
    if (value->unsigned_value > UINT64_MAX - kRegisterEnd)
        return SRH_INVALID;
    floppy.base = value->unsigned_value;
    return SRH_OK;
}

const SrhCardDescriptor descriptor{SRH_INIT(SrhCardDescriptor),
                                   "Storage",
                                   "Floppy drive",
                                   "Removable 1.44 MB block storage with DMA, PIO and "
                                   "observable media changes",
                                   0x1F0,
                                   kRegisterEnd,
                                   0,
                                   0,
                                   0,
                                   0,
                                   R"({"dma_space":"cpu0.memory"})",
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   0};

using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin), "floppy", create, destroy, reset,
                    property_count, property_info, property_get, property_set,
                    State::save, State::load, &descriptor, save_project_data, load_project_data};
} // namespace

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &api : nullptr;
}
