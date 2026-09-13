// Floppy drive tool: host `.img`/`.ima` media through the card's provider
// protocol, without the simulated machine ever learning a host filename.
#include <boundary.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <srz80/tool.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;

constexpr uint32_t kSectorSize = 512;
// Bus activity keeps the lamp lit this long, in simulation time.
constexpr double kActivityWindowNs = 350.0e6;
enum class DialogAction { none, insert, create };

// One floppy drive card as the tool sees it: an owner handle, the card's
// display name, and the opaque state document the card publishes.
struct Drive {
    SrhHandle owner = 0;
    std::string name;
    Json state;
    bool present = false;
};

struct Tool {
    const SrhToolHostV1 *host = nullptr;
    SrhToolRuntime runtime{SRH_INIT(SrhToolRuntime), 0, 0, 1, 0};
    std::vector<Drive> drives;
    SrhHandle selected = 0;
    SrhHandle dialog = 0;
    DialogAction dialog_action = DialogAction::none;
    uint64_t inspect_lba = 0;
    uint64_t seen_access = 0, seen_sequence = UINT64_MAX;
    std::string hex_edit, status, bundle;
    bool write_through = false;

    explicit Tool(const SrhToolHostV1 *source) : host(source) {}

    Drive *current() {
        for (auto &drive : drives)
            if (drive.owner == selected)
                return &drive;
        return nullptr;
    }

    // The provider bundle is the only channel; every card that speaks the
    // floppy protocol appears here with its own state document.
    void refresh() {
        uint64_t size = 0;
        if (host->provider_data(host->context, nullptr, &size) != SRH_OK || !size ||
            size > 16 * 1024 * 1024)
            return;
        std::string text(size, '\0');
        if (host->provider_data(host->context, text.data(), &size) != SRH_OK)
            return;
        text.resize(size ? size - 1 : 0);
        if (text == bundle)
            return;
        const auto root = Json::parse(text, nullptr, false);
        if (root.is_discarded())
            return;
        drives.clear();
        for (const auto &provider : root.value("providers", Json::array())) {
            if (provider.value("protocol", std::string()) != "srz80.floppy.v1")
                continue;
            const auto state = Json::parse(provider.value("data", std::string()), nullptr, false);
            if (state.is_discarded() || state.value("schema", 0) != 1)
                continue;
            Drive drive;
            drive.owner = provider.value("owner", SrhHandle{});
            drive.name = provider.value("display_name", std::string("Floppy drive"));
            drive.present = state.value("media_present", false);
            drive.state = state;
            drives.push_back(std::move(drive));
        }
        bundle = std::move(text);
        const bool still_there = std::any_of(drives.begin(), drives.end(),
                                             [&](const Drive &d) { return d.owner == selected; });
        if (!still_there) {
            selected = drives.empty() ? 0 : drives.front().owner;
            status.clear();
            seen_sequence = UINT64_MAX;
            inspect_lba = 0;
        }
    }

    // A command carries the card's own sequence number, so a card that has
    // moved on rejects a stale request instead of acting on it.
    SrhStatus send(const Json &payload) {
        const Drive *drive = current();
        if (!drive)
            return SRH_NOT_FOUND;
        const std::string text = payload.dump();
        const uint64_t revision = drive->state.value("sequence", uint64_t{0});
        return host->provider_command(host->context, drive->owner, runtime.generation,
                                      SRH_PROVIDER_INTERACTION, revision, text.data(),
                                      text.size());
    }

    static std::string describe(SrhStatus status_code) {
        switch (status_code) {
        case SRH_OK:
            return "ok";
        case SRH_INVALID:
            return "the card rejected the request";
        case SRH_NOT_FOUND:
            return "the drive is no longer present";
        case SRH_UNAVAILABLE:
            return "the file is not a 1.44 MB raw image";
        case SRH_STOP:
            return "the simulation is stopped";
        case SRH_CONFLICT:
            return "the medium is write protected";
        default:
            return "the drive reported status " + std::to_string(status_code);
        }
    }

    void command(const Json &payload, const std::string &action) {
        const auto status_code = send(payload);
        refresh();
        const Drive *drive = current();
        const std::string detail = drive ? drive->state.value("command_error", std::string())
                                         : std::string();
        if (status_code != SRH_OK)
            status = action + " failed: " + (detail.empty() ? describe(status_code) : detail);
        else
            status = action + " completed";
    }

    // The host owns the native file picker; the tool only asks for it and
    // applies the answer once it settles.
    bool has_dialog() const {
        return srz80::sdk::has_field(host, &SrhToolHostV1::file_dialog_request) &&
               host->file_dialog_request &&
               srz80::sdk::has_field(host, &SrhToolHostV1::file_dialog_poll) &&
               host->file_dialog_poll &&
               srz80::sdk::has_field(host, &SrhToolHostV1::file_dialog_release) &&
               host->file_dialog_release;
    }

    void request_image(DialogAction action) {
        if (!has_dialog()) {
            status = "This host does not provide file dialogs";
            return;
        }
        static const SrhToolFileFilter filters[] = {{"Floppy images", "img;ima"},
                                                    {"All files", "*"}};
        if (host->file_dialog_request(host->context, action == DialogAction::create ? 1u : 0u,
                                      filters, 2, &dialog) != SRH_OK) {
            dialog = 0;
            status = "Cannot open the file dialog";
            return;
        }
        dialog_action = action;
    }

    void poll_image() {
        if (!dialog)
            return;
        SrhToolFileResult result{SRH_INIT(SrhToolFileResult), 0, 0, 0};
        auto dialog_status = host->file_dialog_poll(host->context, dialog, &result, nullptr, 0);
        if (dialog_status == SRH_OK && result.pending)
            return;
        std::string path;
        if (dialog_status == SRH_OK && !result.cancelled && result.required_size) {
            std::vector<char> buffer(static_cast<size_t>(result.required_size));
            dialog_status = host->file_dialog_poll(host->context, dialog, &result, buffer.data(),
                                                   buffer.size());
            if (dialog_status == SRH_OK)
                path = buffer.data();
        }
        const DialogAction action = dialog_action;
        host->file_dialog_release(host->context, dialog);
        dialog = 0;
        dialog_action = DialogAction::none;
        if (dialog_status != SRH_OK) {
            status = "The file dialog failed";
            return;
        }
        if (path.empty())
            return; // Cancelled: the drive keeps whatever the user last inserted.
        if (action == DialogAction::create)
            command(Json{{"op", "create"}, {"image", path}}, "Create blank image");
        else
            command(Json{{"op", "insert"}, {"image", path}}, "Insert");
    }

    static std::string basename(const std::string &path) {
        const auto separator = path.find_last_of("/\\");
        return separator == std::string::npos ? path : path.substr(separator + 1);
    }

    static int hex_value(char character) {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        if (character >= 'A' && character <= 'F')
            return character - 'A' + 10;
        return -1;
    }

    // The access lamp.  It is red while the card is working -- a register
    // access, a DMA transfer, or a PIO word -- and fades out over a short
    // window so a burst of activity reads as one blink.  The state comes from
    // the card's own simulation clock, not from wall time.
    float activity_level(const Drive &drive) const {
        const auto accessed = drive.state.value("access_ns", uint64_t{0});
        if (accessed == 0)
            return 0.0f;
        if (accessed != seen_access)
            return 1.0f;
        const auto now = drive.state.value("sim_time_ns", uint64_t{0});
        if (now < accessed)
            return 1.0f;
        const double age = double(now - accessed);
        if (age >= kActivityWindowNs)
            return 0.0f;
        return float(1.0 - age / kActivityWindowNs);
    }

    void draw_lamp(float level) {
        const auto draw = ImGui::GetWindowDrawList();
        const auto origin = ImGui::GetCursorScreenPos();
        constexpr float radius = 7.0f;
        const ImVec2 centre(origin.x + radius + 2.0f, origin.y + radius + 2.0f);
        const bool active = level > 0.02f;
        const ImVec4 colour = active ? ImVec4(1.0f, 0.16f, 0.12f, 0.35f + 0.65f * level)
                                     : ImVec4(0.22f, 0.09f, 0.09f, 1.0f);
        draw->AddCircleFilled(centre, radius, ImGui::GetColorU32(colour), 24);
        draw->AddCircle(centre, radius, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.6f)), 24);
        if (active)
            draw->AddCircleFilled(centre, radius + 3.0f,
                                  ImGui::GetColorU32(ImVec4(1.0f, 0.2f, 0.15f, 0.20f * level)), 24);
        ImGui::Dummy(ImVec2(radius * 2.0f + 6.0f, radius * 2.0f + 6.0f));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lights up on register, DMA, or PIO activity\n"
                              "%llu accesses so far",
                              static_cast<unsigned long long>(
                                  drive_access_count()));
    }

    uint64_t drive_access_count() const {
        for (const auto &drive : drives)
            if (drive.owner == selected)
                return drive.state.value("access_count", uint64_t{0});
        return 0;
    }

    // The sector viewer is a read-only window onto the inserted medium.  The
    // card sends the sector; the tool never opens the image itself.
    void draw_inspector(const Drive &drive) {
        const auto inspect = drive.state.value("inspect", std::string());
        const auto sequence = drive.state.value("sequence", uint64_t{0});
        const auto inspect_sectors = drive.state.value("inspect_sectors", uint64_t{0});
        if (sequence != seen_sequence) {
            seen_sequence = sequence;
            hex_edit = inspect;
        }
        ImGui::Text("Sector %llu", static_cast<unsigned long long>(
                                       drive.state.value("inspect_lba", uint64_t{0})));
        if (hex_edit.size() < size_t(kSectorSize) * 2) {
            ImGui::TextDisabled("No sector data available.");
            return;
        }
        ImGui::BeginChild("sector", ImVec2(0, 220), ImGuiChildFlags_Borders);
        for (uint32_t row = 0; row < kSectorSize / 16; ++row) {
            char line[128];
            int written = std::snprintf(line, sizeof(line), "%04x  ", row * 16);
            for (uint32_t column = 0; column < 16; ++column) {
                const auto offset = size_t(row) * 32 + size_t(column) * 2;
                written += std::snprintf(line + written, sizeof(line) - size_t(written), "%c%c ",
                                         hex_edit[offset], hex_edit[offset + 1]);
                if (column == 7 && written + 1 < int(sizeof(line)))
                    line[written++] = ' ';
            }
            if (written + 4 < int(sizeof(line))) {
                std::memcpy(line + written, " |", 2);
                written += 2;
            }
            for (uint32_t column = 0; column < 16 && written + 1 < int(sizeof(line)); ++column) {
                const auto offset = size_t(row) * 32 + size_t(column) * 2;
                const auto high = hex_value(hex_edit[offset]);
                const auto low = hex_value(hex_edit[offset + 1]);
                const unsigned char character =
                    (high < 0 || low < 0) ? '.' : static_cast<unsigned char>((high << 4) | low);
                line[written++] = (character >= 32 && character < 127) ? char(character) : '.';
            }
            line[written] = '\0';
            ImGui::TextUnformatted(line);
        }
        ImGui::EndChild();
        ImGui::TextDisabled("%llu sectors available",
                            static_cast<unsigned long long>(inspect_sectors));
    }

    void draw(uint32_t *open) {
        bool visible = *open != 0;
        if (!ImGui::Begin("Floppy", &visible)) {
            ImGui::End();
            *open = visible ? 1u : 0u;
            return;
        }
        poll_image();
        refresh();
        if (drives.empty()) {
            ImGui::TextDisabled("No floppy drive");
            ImGui::End();
            *open = visible ? 1u : 0u;
            return;
        }
        const Drive *drive = current();
        const std::string preview = drive ? drive->name : "Select a drive";
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 40.0f);
        if (ImGui::BeginCombo("###drive", preview.c_str())) {
            for (const auto &candidate : drives) {
                if (ImGui::Selectable(candidate.name.c_str(), candidate.owner == selected)) {
                    selected = candidate.owner;
                    hex_edit.clear();
                    seen_sequence = UINT64_MAX;
                    inspect_lba = 0;
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (!drive) {
            ImGui::End();
            *open = visible ? 1u : 0u;
            return;
        }
        const float level = activity_level(*drive);
        seen_access = drive->state.value("access_ns", uint64_t{0});
        ImGui::SameLine();
        draw_lamp(level);

        const bool present = drive->state.value("media_present", false);
        const auto media_id = drive->state.value("media_id", uint64_t{0});
        const auto sectors = drive->state.value("sectors", uint64_t{0});
        const auto sector_size = drive->state.value("sector_size", uint64_t{0});
        const auto capacity = drive->state.value("capacity", uint64_t{0});
        const bool protected_media = drive->state.value("write_protected", false);
        const auto image = drive->state.value("image", std::string());

        ImGui::SeparatorText("Media");
        if (present) {
            ImGui::Text("Disk:     %s", basename(image).c_str());
            ImGui::Text("Size:     %.2f MB (%llu sectors x %llu bytes)",
                        double(capacity) / (1024.0 * 1024.0),
                        static_cast<unsigned long long>(sectors),
                        static_cast<unsigned long long>(sector_size));
            ImGui::Text("Writable: %s", protected_media ? "No (host image is read-only)" : "Yes");
        } else {
            ImGui::TextDisabled("No disk");
        }
        ImGui::Text("MEDIA_ID: %llu", static_cast<unsigned long long>(media_id));
        const bool changed = drive->state.value("media_changed", false);
        ImGui::SameLine();
        ImGui::TextColored(changed ? ImVec4(0.9f, 0.75f, 0.3f, 1.0f)
                                   : ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
                           changed ? "MEDIA_CHANGED is latched" : "media change acknowledged");
        const auto error = drive->state.value("error", uint64_t{0});
        if (error != 0) {
            ImGui::TextColored(ImVec4(0.9f, 0.45f, 0.4f, 1.0f), "ERROR: %s",
                               drive->state.value("error_name", std::string("UNKNOWN")).c_str());
        }
        if (drive->state.value("busy", false))
            ImGui::TextDisabled("Busy");

        ImGui::SeparatorText("Actions");
        ImGui::BeginDisabled(dialog != 0 || !has_dialog());
        if (ImGui::Button("Insert..."))
            request_image(DialogAction::insert);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(dialog != 0 || !has_dialog());
        if (ImGui::Button("New blank image..."))
            request_image(DialogAction::create);
        ImGui::EndDisabled();
        if (dialog != 0)
            ImGui::SameLine(), ImGui::TextDisabled("Opening...");
        ImGui::SameLine();
        ImGui::BeginDisabled(!present);
        if (ImGui::Button("Eject"))
            command(Json{{"op", "eject"}}, "Eject");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!present);
        if (ImGui::Button("Flush"))
            command(Json{{"op", "flush"}}, "Flush");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!present);
        if (ImGui::Button("Verify image"))
            command(Json{{"op", "verify"}}, "Verify");
        ImGui::EndDisabled();
        // Inserting and ejecting works in every run state.  While the machine is
        // not running nobody is watching the bus, so the guest cannot notice the
        // change until it runs again.
        if (!runtime.running)
            ImGui::TextDisabled(runtime.stopped ? "Rack stopped" : "Rack paused");
        if (ImGui::Checkbox("Write through the host image", &write_through)) {
            if (srz80::sdk::has_field(host, &SrhToolHostV1::config_set) && host->config_set)
                host->config_set(host->context, "floppy.write_through", write_through ? "1" : "0");
            command(Json{{"op", "configure"}, {"flush_after_write", write_through}},
                    "Write-through");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off: saves when you flush or eject\nOn: saves after every write");

        if (present) {
            ImGui::SeparatorText("Sector inspector");
            ImGui::TextDisabled("Host image: %s", image.c_str());
            int lba = int(inspect_lba);
            if (ImGui::InputInt("Sector", &lba, 1, 16)) {
                inspect_lba = uint64_t(std::max(0, lba));
                command(Json{{"op", "seek"}, {"lba", inspect_lba}}, "Show sector");
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload"))
                command(Json{{"op", "seek"}, {"lba", inspect_lba}}, "Show sector");
            draw_inspector(*drive);
            if (ImGui::Button("Patch sector"))
                command(Json{{"op", "write_sector"}, {"lba", inspect_lba}, {"hex", hex_edit}},
                        "Patch sector");
            ImGui::SameLine();
            if (ImGui::Button("Discard edit"))
                hex_edit.clear();
        }

        if (!status.empty())
            ImGui::TextWrapped("%s", status.c_str());
        ImGui::End();
        *open = visible ? 1u : 0u;
    }

    void tick(uint32_t visible) {
        (void)visible;
        SrhToolRuntime updated{SRH_INIT(SrhToolRuntime), 0, 0, 0, 0};
        if (host->runtime_info(host->context, &updated) != SRH_OK)
            return;
        if (updated.generation != runtime.generation) {
            drives.clear();
            bundle.clear();
            selected = 0;
            hex_edit.clear();
            status.clear();
            inspect_lba = 0;
            seen_sequence = UINT64_MAX;
            seen_access = 0;
        }
        runtime = updated;
        refresh();
    }
};

SrhStatus SRH_CALL create(const SrhToolHostV1 *host, void **out) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !out || !host->provider_data || !host->provider_command ||
            !host->runtime_info || !host->imgui_version ||
            std::string(host->imgui_version) != IMGUI_VERSION || !host->imgui_context ||
            !host->imgui_alloc || !host->imgui_free)
            return SRH_INVALID;
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(host->imgui_context));
        ImGui::SetAllocatorFunctions(host->imgui_alloc, host->imgui_free,
                                     host->imgui_allocator_context);
        auto tool = std::make_unique<Tool>(host);
        if (srz80::sdk::has_field(host, &SrhToolHostV1::config_get) && host->config_get) {
            char initial[16]{};
            if (host->config_get(host->context, "floppy.write_through", initial, sizeof(initial)) ==
                SRH_OK)
                tool->write_through = std::strcmp(initial, "1") == 0;
        }
        if (srz80::sdk::has_field(host, &SrhToolHostV1::config_register) && host->config_register) {
            SrhConfigEntry entry{SRH_INIT(SrhConfigEntry),
                                 "Tools/Floppy",
                                 "floppy.write_through",
                                 "Write through the host image",
                                 "On: saves every successful write right away. Off: saves on flush or eject",
                                 Srh_CONFIG_BOOL,
                                 nullptr,
                                 "0",
                                 tool.get(),
                                 0,
                                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                                     if (!value || !capacity)
                                         return SRH_INVALID;
                                     std::snprintf(value, capacity, "%d",
                                                   static_cast<Tool *>(context)->write_through ? 1
                                                                                               : 0);
                                     return SRH_OK;
                                 },
                                 [](void *context, const char *value) -> SrhStatus {
                                     static_cast<Tool *>(context)->write_through =
                                         value && std::strcmp(value, "1") == 0;
                                     return SRH_OK;
                                 }};
            host->config_register(host->context, &entry);
        }
        *out = tool.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *instance) { delete static_cast<Tool *>(instance); }

SrhStatus SRH_CALL draw(void *instance, uint32_t *open) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!instance || !open)
            return SRH_INVALID;
        auto &tool = *static_cast<Tool *>(instance);
        ImGui::SetCurrentContext(static_cast<ImGuiContext *>(tool.host->imgui_context));
        tool.draw(open);
        return SRH_OK;
    });
}

SrhStatus SRH_CALL tick(void *instance, uint32_t visible) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!instance)
            return SRH_INVALID;
        static_cast<Tool *>(instance)->tick(visible);
        return SRH_OK;
    });
}

const SrhToolPlugin api{SRH_INIT(SrhToolPlugin), "floppy", "Floppy", IMGUI_VERSION,
                        create, destroy, draw, "Storage",
                        Srh_TOOL_PROJECT_STATE_TEXT, nullptr, nullptr, nullptr, tick};
} // namespace

extern "C" SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *host) {
    return host && host->abi_version == SRH_ABI &&
                   srz80::sdk::has_field(host, &SrhToolHostV1::provider_command)
               ? &api
               : nullptr;
}
