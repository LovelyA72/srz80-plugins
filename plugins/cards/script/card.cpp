#include "script_card.h"

namespace srz80_script {

SrhStatus ScriptCard::read_project(std::string_view relative, std::vector<uint8_t> &out) {
    if (!files || !files->read_file)
        return SRH_UNAVAILABLE;
    std::string normalized;
    if (!normalize_project_path(relative, normalized))
        return SRH_INVALID;
    uint64_t size = 0;
    const SrhStatus query_status = files->read_file(files->context, owner, normalized.c_str(),
                                                    nullptr, &size);
    if (query_status != SRH_OK)
        return query_status;
    if (size > kFileLimit || size > std::numeric_limits<size_t>::max())
        return SRH_INVALID;
    out.clear();
    if (!size)
        return SRH_OK;
    out.resize(static_cast<size_t>(size));
    uint64_t capacity = size;
    const SrhStatus read_status = files->read_file(files->context, owner, normalized.c_str(),
                                                   out.data(), &capacity);
    if (read_status != SRH_OK) {
        out.clear();
        return read_status;
    }
    if (capacity > size) {
        out.clear();
        return SRH_ERROR;
    }
    out.resize(static_cast<size_t>(capacity));
    return SRH_OK;
}

bool ScriptCard::sources_changed() {
    if (!vm)
        return false;
    for (const auto &[path, cached] : vm->source_cache) {
        std::vector<uint8_t> bytes;
        if (read_project(path, bytes) != SRH_OK)
            return true;
        const char *data = bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data());
        if (trim_bom(std::string(data, bytes.size())) != cached)
            return true;
    }
    for (const auto &[path, cached] : vm->file_snapshots) {
        std::vector<uint8_t> bytes;
        if (read_project(path, bytes) != SRH_OK)
            return true;
        const char *data = bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data());
        if (file_fingerprint(std::string_view(data, bytes.size())) != cached)
            return true;
    }
    return false;
}

SrhStatus ScriptCard::write_project(std::string_view relative, const uint8_t *data,
                                    uint64_t byte_count) {
    if (!files || !files->write_file)
        return SRH_UNAVAILABLE;
    if (byte_count > kFileLimit || (byte_count && !data) ||
        byte_count > std::numeric_limits<size_t>::max())
        return SRH_INVALID;
    std::string normalized;
    if (!normalize_project_path(relative, normalized))
        return SRH_INVALID;
    return files->write_file(files->context, owner, normalized.c_str(), data, byte_count);
}

bool ScriptCard::lookup_space(std::string_view name, SrhHandle &handle) {
    const std::string key(name);
    if (auto found = spaces.find(key); found != spaces.end()) {
        handle = found->second;
        return true;
    }
    if (!resources || !resources->lookup)
        return false;
    if (resources->lookup(resources->context, "space", key.c_str(), &handle) != SRH_OK)
        return false;
    spaces.emplace(key, handle);
    return true;
}

bool ScriptCard::lookup_signal(std::string_view name, SrhHandle &handle) {
    const std::string key(name);
    if (auto found = signals.find(key); found != signals.end()) {
        handle = found->second;
        return true;
    }
    if (!host || !host->signal_find ||
        host->signal_find(host->context, key.c_str(), &handle) != SRH_OK)
        return false;
    signals.emplace(key, handle);
    return true;
}

std::unique_ptr<ScriptVm> ScriptCard::make_vm(std::string_view path, std::string &error,
                                              bool should_activate) {
    if (path.empty()) {
        error = "No main script file selected";
        return {};
    }
    const fs::path input = path_from_utf8(path);
    const fs::path selected = (input.is_absolute() ? input : project_root / input)
                                  .lexically_normal();
    if (!path_is_within(project_root, selected)) {
        error = "Main script must be inside the active project folder";
        return {};
    }
    const std::string ext = [&] {
        std::string value = path_utf8(selected.extension());
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }();
    Backend backend;
    if (ext == ".lua")
        backend = Backend::lua;
    else if (ext == ".js")
        backend = Backend::javascript;
    else {
        error = "Unsupported main script extension (choose .lua or .js)";
        return {};
    }
    const fs::path relative = selected.lexically_relative(project_root);
    std::string relative_text = path_utf8(relative);
    std::replace(relative_text.begin(), relative_text.end(), '\\', '/');
    std::string normalized;
    if (!normalize_project_path(relative_text, normalized)) {
        error = "Main script path is not relative to the active project folder";
        return {};
    }
    std::vector<uint8_t> bytes;
    const SrhStatus status = read_project(normalized, bytes);
    if (status != SRH_OK) {
        error = "Cannot load main script " + normalized + ": " +
                ScriptVm::status_text(status);
        return {};
    }
    if (bytes.size() > kSourceLimit) {
        error = "Main script exceeds the 16 MiB source limit";
        return {};
    }
    std::unique_ptr<ScriptVm> candidate;
    if (backend == Backend::lua)
        candidate = std::make_unique<LuaVm>(*this, normalized);
    else
        candidate = std::make_unique<JsVm>(*this, normalized);
    // Seed the source cache so the main file is counted once and module loaders
    // can resolve relative imports against the normalized project path.
    const char *source_bytes = bytes.empty() ? "" : reinterpret_cast<const char *>(bytes.data());
    std::string source_text(source_bytes, bytes.size());
    source_text = trim_bom(std::move(source_text));
    candidate->loaded_source_bytes = bytes.size();
    candidate->loaded_paths.insert(normalized);
    candidate->source_cache.emplace(normalized, std::move(source_text));
    if (!candidate->initialize(error))
        return {};
    if (should_activate && !candidate->activate(error))
        return {};
    return candidate;
}

namespace {

void set_create_error(const SrhConfig *config, std::string_view message) {
    if (!config || !srz80::sdk::has_field(config, &SrhConfig::error_message_capacity) ||
        !config->error_message || !config->error_message_capacity)
        return;
    const size_t amount = std::min<size_t>(message.size(), config->error_message_capacity - 1);
    std::memcpy(config->error_message, message.data(), amount);
    config->error_message[amount] = '\0';
}

bool query_project_root(ScriptCard &card, std::string &error) {
    if (!card.host->query ||
        card.host->query(card.host->context, "host.project_files.v1",
                         reinterpret_cast<const void **>(&card.files)) != SRH_OK ||
        !card.files) {
        error = "Host project-files extension is unavailable";
        return false;
    }
    if (card.files->abi_version != SRH_ABI ||
        card.files->struct_size < sizeof(SrhHostProjectFilesV1) ||
        !card.files->project_root || !card.files->read_file || !card.files->write_file) {
        error = "Host project-files extension has an incompatible ABI";
        return false;
    }
    uint64_t size = 0;
    const SrhStatus query = card.files->project_root(card.files->context, card.owner,
                                                     nullptr, &size);
    if (query != SRH_OK) {
        error = query == SRH_UNAVAILABLE
            ? "No active project folder is available"
            : "Cannot query the active project folder";
        return false;
    }
    if (size < 2 || size > 32768) {
        error = "Host returned an invalid project folder path size";
        return false;
    }
    std::vector<char> path(static_cast<size_t>(size), '\0');
    uint64_t capacity = size;
    const SrhStatus result = card.files->project_root(card.files->context, card.owner,
                                                      path.data(), &capacity);
    if (result != SRH_OK || capacity != size || path.back() != '\0' ||
        std::strlen(path.data()) + 1 != path.size()) {
        error = "Cannot read the active project folder path";
        return false;
    }
    const std::string utf8(path.data(), path.size() - 1);
    if (utf8.empty()) {
        error = "Host returned an invalid project folder path";
        return false;
    }
    card.project_root = path_from_utf8(utf8).lexically_normal();
    if (!card.project_root.is_absolute()) {
        error = "Host returned a non-absolute project folder";
        return false;
    }
    return true;
}

SrhStatus error_status(std::string_view error) {
    if (error.find("not found") != std::string_view::npos ||
        error.find("file not found") != std::string_view::npos)
        return SRH_NOT_FOUND;
    if (error.find("unavailable") != std::string_view::npos ||
        error.find("No active project") != std::string_view::npos)
        return SRH_UNAVAILABLE;
    if (error.find("Unsupported") != std::string_view::npos ||
        error.find("invalid") != std::string_view::npos ||
        error.find("must be") != std::string_view::npos)
        return SRH_INVALID;
    return SRH_ERROR;
}

void copy_text(char *target, size_t capacity, std::string_view source) {
    if (!target || !capacity)
        return;
    const size_t size = std::min(capacity - 1, source.size());
    if (size)
        std::memcpy(target, source.data(), size);
    target[size] = '\0';
}

SrhStatus SRH_CALL read(void *opaque, uint64_t address, uint8_t *value) {
    return srz80::sdk::guard([&]() -> SrhStatus {
    auto *card = static_cast<ScriptCard *>(opaque);
    if (!card || !value || address < card->base || address - card->base >= card->size)
        return SRH_INVALID;
    if (!card->vm) {
        *value = 0;
        return SRH_OK;
    }
    std::string error;
    if (!card->vm->call_read(address, *value, error)) {
        card->vm->log_error(card->vm->main_relative, error);
        *value = 0;
        return SRH_ERROR;
    }
    return SRH_OK;
    });
}

SrhStatus SRH_CALL write(void *opaque, uint64_t address, uint8_t value) {
    return srz80::sdk::guard([&]() -> SrhStatus {
    auto *card = static_cast<ScriptCard *>(opaque);
    if (!card || address < card->base || address - card->base >= card->size)
        return SRH_INVALID;
    if (!card->vm)
        return SRH_OK;
    std::string error;
    if (!card->vm->call_write(address, value, error)) {
        card->vm->log_error(card->vm->main_relative, error);
        return SRH_ERROR;
    }
    return SRH_OK;
    });
}

SrhStatus SRH_CALL peek(void *, uint64_t, uint8_t *) {
    // A script read hook can mutate arbitrary VM state, so the card never runs
    // script code for a side-effect-free peek.
    return SRH_UNAVAILABLE;
}

SrhStatus SRH_CALL resume_script(void *opaque) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        auto *card = static_cast<ScriptCard *>(opaque);
        if (!card || !card->vm || card->main_file.empty() ||
            (!card->restart_pending && !card->sources_changed()))
            return SRH_OK;
        auto *previous = card->vm.get();
        previous->deactivate();
        std::string error;
        auto candidate = card->make_vm(card->main_file, error);
        if (candidate && !candidate->call_reset(true, error))
            candidate.reset();
        if (!candidate) {
            card->restart_pending = true;
            std::string restore_error;
            if (!previous->activate(restore_error))
                error += "; cannot restore previous script: " + restore_error;
            card->log("script reload failed: " + error);
            return SRH_ERROR;
        }
        card->main_relative = candidate->main_relative;
        card->vm = std::move(candidate);
        card->restart_pending = false;
        card->log("script sources reloaded");
        return SRH_OK;
    });
}

SrhStatus SRH_CALL create(const ShouryoHost *host, SrhHandle owner,
                          const SrhConfig *config, void **result) {
    return srz80::sdk::guard([&]() -> SrhStatus {
        if (!srz80::sdk::valid(host) || !srz80::sdk::valid(config) || !result ||
            !host->map || !host->query || !config->size ||
            config->size > kSourceLimit ||
            config->base > UINT64_MAX - (config->size - 1)) {
            set_create_error(config, "Invalid host, configuration, or script memory range");
            return SRH_INVALID;
        }
        auto card = std::make_unique<ScriptCard>();
        card->host = host;
        card->owner = owner;
        card->base = config->base;
        card->size = config->size;
        if (host->query(host->context, "host.resources.v1",
                        reinterpret_cast<const void **>(&card->resources)) != SRH_OK ||
            !card->resources || card->resources->abi_version != SRH_ABI ||
            card->resources->struct_size < sizeof(SrhHostResourcesV1) ||
            !card->resources->lookup) {
            set_create_error(config, "Host resource lookup extension is unavailable");
            return SRH_UNAVAILABLE;
        }
        const void *lifecycle = nullptr;
        if (host->query(host->context, "host.lifecycle.v1", &lifecycle) == SRH_OK && lifecycle) {
            const auto *extension = static_cast<const SrhHostLifecycleV1 *>(lifecycle);
            if (extension->abi_version == SRH_ABI &&
                extension->struct_size >= sizeof(SrhHostLifecycleV1) &&
                extension->subscribe_resume)
                card->lifecycle = extension;
        }
        std::string error;
        if (!query_project_root(*card, error)) {
            set_create_error(config, error);
            return error_status(error);
        }
        std::string main_file;
        if (srz80::sdk::has_field(config, &SrhConfig::config_json) &&
            config->config_json && config->config_json_size) {
            if (config->config_json_size > 64 * 1024 ||
                !valid_utf8_text(config->config_json,
                                 static_cast<size_t>(config->config_json_size))) {
                set_create_error(config, "Card configuration JSON is too large or invalid");
                return SRH_INVALID;
            }
            Json settings;
            try {
                settings = Json::parse(config->config_json,
                                       config->config_json + config->config_json_size);
            } catch (...) {
                set_create_error(config, "Card configuration must be valid JSON");
                return SRH_INVALID;
            }
            bool unknown_setting = false;
            if (settings.is_object()) {
                for (auto it = settings.begin(); it != settings.end(); ++it)
                    unknown_setting |= it.key() != "main_file";
            }
            if (!settings.is_object() || unknown_setting) {
                set_create_error(config, "Card configuration accepts only the main_file setting");
                return SRH_INVALID;
            }
            if (settings.contains("main_file")) {
                if (!settings["main_file"].is_string()) {
                    set_create_error(config, "main_file must be a string path");
                    return SRH_INVALID;
                }
                main_file = settings["main_file"].get<std::string>();
                if (main_file.size() >= sizeof(SrhValue::text) ||
                    main_file.find('\0') != std::string::npos) {
                    set_create_error(config, "main_file path must fit in 127 UTF-8 bytes");
                    return SRH_INVALID;
                }
            }
        }
        card->main_file = main_file;
        if (!main_file.empty()) {
            auto vm = card->make_vm(main_file, error);
            if (!vm) {
                set_create_error(config, error);
                return error_status(error);
            }
            card->main_relative = vm->main_relative;
            card->vm = std::move(vm);
        }

        SrhMapping mapping{SRH_INIT(SrhMapping),
                           config->space,
                           config->base,
                           config->base + config->size - 1,
                           config->priority,
                           card.get(),
                           read,
                           write,
                           peek,
                           nullptr};
        SrhHandle handle = 0;
        const SrhStatus mapped = host->map(host->context, owner, &mapping, &handle);
        if (mapped != SRH_OK) {
            set_create_error(config, "Could not register the script card memory range");
            return mapped;
        }
        card->mapping = handle;
        if (card->lifecycle) {
            const SrhStatus subscribed = card->lifecycle->subscribe_resume(
                card->lifecycle->context, owner, resume_script, card.get(),
                &card->resume_subscription);
            if (subscribed != SRH_OK)
                return subscribed;
        }
        *result = card.release();
        return SRH_OK;
    });
}

void SRH_CALL destroy(void *opaque) {
    auto *card = static_cast<ScriptCard *>(opaque);
    if (card && card->resume_subscription && card->host && card->host->cancel)
        card->host->cancel(card->host->context, card->resume_subscription);
    delete card;
}

SrhStatus SRH_CALL reset(void *opaque, uint32_t cold) {
    return srz80::sdk::guard([&]() -> SrhStatus {
    auto *card = static_cast<ScriptCard *>(opaque);
    if (!card)
        return SRH_INVALID;
    if (cold)
        card->restart_pending = true;
    if (!card->vm)
        return SRH_OK;
    if (cold && card->lifecycle) {
        card->vm->deactivate();
        return SRH_OK;
    }
    auto *previous = card->vm.get();
    previous->deactivate();
    std::string error;
    auto candidate = card->make_vm(card->main_file, error);
    if (candidate && !candidate->call_reset(cold != 0, error))
        candidate.reset();
    if (!candidate) {
        std::string restore_error;
        if (!previous->activate(restore_error))
            error += "; cannot restore previous script: " + restore_error;
        card->restart_pending = true;
        card->log("script reset failed: " + error);
        return SRH_ERROR;
    }
    card->main_relative = candidate->main_relative;
    card->vm = std::move(candidate);
    card->restart_pending = false;
    return SRH_OK;
    });
}

uint32_t SRH_CALL property_count(void *) {
    return 1;
}

SrhStatus SRH_CALL property_info(void *, uint32_t index, SrhProperty *property) {
    if (index || !srz80::sdk::valid(property))
        return SRH_NOT_FOUND;
    *property = {SRH_INIT(SrhProperty),
                 "main_file",
                 "Script",
                 "Project main source (.lua or .js)",
                 SRH_TEXT,
                 0,
                 0,
                 1,
                 nullptr,
                 SRH_PROPERTY_PERSISTENT};
    return SRH_OK;
}

SrhStatus SRH_CALL property_get(void *opaque, uint32_t index, SrhValue *value) {
    auto *card = static_cast<ScriptCard *>(opaque);
    if (!card || index || !srz80::sdk::valid(value))
        return SRH_INVALID;
    value->text[0] = '\0';
    if (card->main_file.size() >= sizeof(value->text))
        return SRH_ERROR;
    copy_text(value->text, sizeof(value->text), card->main_file);
    return SRH_OK;
}

SrhStatus SRH_CALL property_set(void *opaque, uint32_t index, const SrhValue *value) {
    return srz80::sdk::guard([&]() -> SrhStatus {
    auto *card = static_cast<ScriptCard *>(opaque);
    if (!card || index || !srz80::sdk::valid(value))
        return SRH_INVALID;
    const void *end = std::memchr(value->text, '\0', sizeof(value->text));
    if (!end)
        return SRH_INVALID;
    const size_t length = static_cast<const char *>(end) - value->text;
    const std::string path(value->text, length);
    if (path.size() > 127)
        return SRH_INVALID;
    if (path == card->main_file)
        return SRH_OK;
    if (path.empty()) {
        card->vm.reset();
        card->main_file.clear();
        card->main_relative.clear();
        card->restart_pending = false;
        return SRH_OK;
    }
    std::string error;
    auto candidate = card->make_vm(path, error, false);
    if (!candidate) {
        card->log("script main_file update failed: " + error);
        return error_status(error);
    }
    if (card->vm)
        card->vm->deactivate();
    if (!candidate->activate(error)) {
        if (card->vm) {
            std::string restore_error;
            if (!card->vm->activate(restore_error))
                error += "; cannot restore previous script: " + restore_error;
        }
        card->log("script main_file update failed: " + error);
        return error_status(error);
    }
    card->main_relative = candidate->main_relative;
    card->main_file = path;
    card->vm = std::move(candidate);
    return SRH_OK;
    });
}

SrhStatus SRH_CALL save_payload(void *opaque, uint8_t *buffer, uint64_t *size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
    auto *card = static_cast<ScriptCard *>(opaque);
    if (!card || !size)
        return SRH_INVALID;
    std::string json = "{}";
    if (card->vm) {
        std::string error;
        if (!card->vm->save_user_state(json, error)) {
            card->vm->log_error(card->vm->main_relative, error);
            return SRH_INVALID;
        }
    }
    return srz80::sdk::state::copy_payload(
        {reinterpret_cast<const uint8_t *>(json.data()), json.size()}, buffer, size);
    });
}

SrhStatus SRH_CALL load_payload(void *opaque, const uint8_t *buffer, uint64_t size) {
    return srz80::sdk::guard([&]() -> SrhStatus {
    auto *card = static_cast<ScriptCard *>(opaque);
    if (!card || (!buffer && size) || size > kStateLimit)
        return SRH_INVALID;
    const char *data = size ? reinterpret_cast<const char *>(buffer) : "";
    const std::string_view json(data, static_cast<size_t>(size));
    try {
        const Json value = Json::parse(json.begin(), json.end());
        if (!value.is_object())
            return SRH_INVALID;
    } catch (...) {
        return SRH_INVALID;
    }
    if (!card->vm)
        return json == "{}" ? SRH_OK : SRH_INVALID;
    std::string error;
    if (!card->vm->load_user_state(json, error)) {
        card->vm->log_error(card->vm->main_relative, error);
        return SRH_INVALID;
    }
    return SRH_OK;
    });
}

const SrhCardDescriptor descriptor{
    SRH_INIT(SrhCardDescriptor),
    "Automation",
    "Script",
    "Run a Lua 5.5 or QuickJS-NG project script",
    0xF000,
    256,
    0,
    0,
    0,
    0,
    R"({"main_file":""})",
    nullptr,
    nullptr,
    nullptr,
    0};
using State = srz80::sdk::state::Callbacks<save_payload, load_payload, 1>;
const SrhPlugin api{SRH_INIT(SrhPlugin),
                    "script",
                    create,
                    destroy,
                    reset,
                    property_count,
                    property_info,
                    property_get,
                    property_set,
                    State::save,
                    State::load,
                    &descriptor,
                    nullptr,
                    nullptr};

} // namespace

} // namespace srz80_script

extern "C" SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host) {
    return srz80::sdk::valid(host) ? &srz80_script::api : nullptr;
}
