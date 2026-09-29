#include "project_settings.hpp"
#include <srz80/tool.h>
#include <imgui.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace srz80::assembler;
namespace {
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
struct Host {
    SrhToolHostV1 api{};
    std::vector<SrhToolTextFormat> formats;
    std::string root = "/projects/one";
    std::vector<uint8_t> loaded;
    uint64_t address = 0;
    unsigned loads = 0;
    Host() {
        api.abi_version = SRH_ABI;
        api.struct_size = sizeof(api);
        api.context = this;
        api.imgui_context = ImGui::GetCurrentContext();
        api.imgui_version = IMGUI_VERSION;
        api.imgui_alloc = [](size_t size, void *) { return std::malloc(size); };
        api.imgui_free = [](void *pointer, void *) { std::free(pointer); };
        api.space_count = [](void *) -> uint32_t { return 1; };
        api.space_info = [](void *, uint32_t index, SrhToolSpace *space) -> SrhStatus {
            if (index) return SRH_INVALID;
            space->id = 1;
            space->maximum = 0xffffff;
            std::strcpy(space->name, "memory");
            return SRH_OK;
        };
        api.run_state = [](void *) -> uint32_t { return SRT_STOPPED; };
        api.run = [](void *) -> SrhStatus { return SRH_OK; };
        api.load_memory_segments = [](void *p, SrhHandle, const SrhToolMemorySegment *segments,
                                      uint32_t count, uint32_t, uint32_t *failed, uint64_t *written) -> SrhStatus {
            auto &host = *static_cast<Host *>(p);
            host.loaded.clear();
            ++host.loads;
            check(count != 0, "empty load");
            host.address = segments[0].address;
            for (uint32_t i = 0; i < count; ++i)
                host.loaded.insert(host.loaded.end(), segments[i].bytes, segments[i].bytes + segments[i].size);
            *written = host.loaded.size();
            *failed = UINT32_MAX;
            return SRH_OK;
        };
        api.text_format_register = [](void *p, const SrhToolTextFormat *format) -> SrhStatus {
            static_cast<Host *>(p)->formats.push_back(*format);
            return SRH_OK;
        };
        api.text_format_unregister = [](void *p, void *context) -> SrhStatus {
            std::erase_if(static_cast<Host *>(p)->formats,
                          [&](auto &format) { return format.handler_context == context; });
            return SRH_OK;
        };
        api.project_text_update = [](void *, void *, const char *, const char *, uint64_t, uint64_t) -> SrhStatus {
            return SRH_OK;
        };
        api.project_root = [](void *p, char *out, uint64_t *size) -> SrhStatus {
            const auto &root = static_cast<Host *>(p)->root;
            auto capacity = *size;
            *size = root.size() + 1;
            if (!out) return SRH_OK;
            if (capacity < *size) return SRH_INVALID;
            std::memcpy(out, root.c_str(), *size);
            return SRH_OK;
        };
    }
    void open(const char *name, const char *source) {
        check(!formats.empty(), "source handler not registered");
        const auto &format = formats.front();
        const auto path = root + "/" + name;
        check(format.open(format.handler_context, path.c_str(), source, std::strlen(source), 0) == SRH_OK,
              "source open failed");
    }
};
std::string state(const SrhToolPlugin *api, void *tool) {
    uint64_t size = 0;
    check(api->project_state_get(tool, nullptr, &size) == SRH_OK && size > 1, "state size query failed");
    std::string text(size, '\0');
    check(api->project_state_get(tool, text.data(), &size) == SRH_OK && text.back() == '\0', "state get failed");
    text.pop_back();
    return text;
}
void frame(const SrhToolPlugin *api, void *tool, bool load_key = false) {
    auto &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_F6, load_key);
    ImGui::NewFrame();
    ImGui::SetNextWindowFocus();
    uint32_t open = 1;
    check(api->draw(tool, &open) == SRH_OK, "tool draw failed");
    ImGui::Render();
}
void load(Host &host, const SrhToolPlugin *api, void *tool,
          uint64_t address, const std::vector<uint8_t> &expected) {
    frame(api, tool);
    const auto before = host.loads;
    frame(api, tool, true);
    check(host.loads == before + 1, "F6 did not load assembled source");
    check(host.address == address && host.loaded == expected, "restored target/origin emitted wrong bytes");
    frame(api, tool);
}
void run() {
    auto *context = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1200, 900);
    io.DeltaTime = 1.f / 60.f;
    unsigned char *pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    Host host;
    const auto *api = srz80_tool_init(&host.api);
    check(api && std::string(api->id) == "z80_assembler", "plugin identity changed");
    check(api->flags & Srh_TOOL_PROJECT_STATE_TEXT, "missing structured project state flag");
    void *tool = nullptr;
    check(api->create(&host.api, &tool) == SRH_OK && tool, "tool create failed");
    check(host.formats.size() == 6, "source format registration");
    for (const auto &format : host.formats) check(format.flags == 0, "source registered as binary");
    ProjectSettings settings;
    settings.sources["main.asm"] = {Target::mc68000, 0x120000};
    settings.sources["bank.asm"] = {Target::w65c816, 0x128000};
    settings.sources["small.asm"] = {Target::w65c02, 0x8000};
    check(api->project_state_load(tool, settings.save().c_str()) == SRH_OK, "state restore failed");
    host.open("main.asm", "moveq #1,d0\nrts");
    load(host, api, tool, 0x120000, {0x70,1,0x4e,0x75});
    host.open("bank.asm", ".al\nlda #$1234\nrtl");
    load(host, api, tool, 0x128000, {0xa9,0x34,0x12,0x6b});
    host.open("small.asm", "stz $1234\nwai");
    load(host, api, tool, 0x8000, {0x9c,0x34,0x12,0xcb});
    const auto saved = state(api, tool);
    check(saved == settings.save(), "opening sources lost saved targets");
    char tiny = 'x';
    uint64_t size = 1;
    check(api->project_state_get(tool, &tiny, &size) != SRH_OK && tiny == 'x', "short state buffer overwritten");
    check(api->project_state_load(tool, "{invalid") != SRH_OK, "invalid state accepted");
    check(state(api, tool) == saved, "invalid state destroyed settings");
    api->destroy(tool);
    check(host.formats.empty(), "handlers not unregistered");
    host.root = "/projects/moved";
    check(api->create(&host.api, &tool) == SRH_OK, "recreate failed");
    check(api->project_state_load(tool, saved.c_str()) == SRH_OK, "fresh tool restore failed");
    host.open("bank.asm", "lda #$12\nrtl"); // width state must not survive another assembly
    load(host, api, tool, 0x128000, {0xa9,0x12,0x6b});
    host.open("main.asm", "nop");
    load(host, api, tool, 0x120000, {0x4e,0x71});
    host.open("new.asm", "ld a,1");
    load(host, api, tool, 0, {0x3e,1});
    check(api->project_state_load(tool, "") == SRH_OK, "new project reset failed");
    const auto before = host.loads;
    frame(api, tool, true);
    check(host.loads == before, "old source remained loadable after project replacement");
    frame(api, tool);
    host.open("main.asm", "nop");
    load(host, api, tool, 0, {0});
    check(api->project_state_load(tool, "main.asm") == SRH_OK, "legacy project rejected");
    host.open("main.asm", "nop");
    load(host, api, tool, 0, {0});
    api->destroy(tool);
    ImGui::DestroyContext(context);
}
}
int main() {
    try { run(); std::cout << "Assembler plugin project/load checks passed\n"; }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
