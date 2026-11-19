#include "native_artifact.h"

#include "modfile.h"
#include "platform.h"
#include "runtime_state.h"
#include "sha256.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

constexpr const char *kMetadataName = "artifact.meta";
constexpr const char *kMetadataVersion = "1";

std::string trim_copy(const std::string &text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace((unsigned char)text[begin]))
        ++begin;
    std::size_t end = text.size();
    while (end > begin &&
           std::isspace((unsigned char)text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

std::string shell_quote(std::string text) {
    for (char &c : text) {
        if (c == '\\')
            c = '/';
    }
    std::string out = "\"";
    for (char c : text) {
        if (c == '"')
            out += "\\\"";
        else
            out.push_back(c);
    }
    out += '"';
    return out;
}

std::string first_line(std::string text) {
    std::string result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::string line = trim_copy(text.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin));
        if (!line.empty()) {
            result = line;
            break;
        }
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    std::string compact;
    bool space = false;
    for (char c : result) {
        if (std::isspace((unsigned char)c)) {
            space = !compact.empty();
            continue;
        }
        if (space)
            compact.push_back(' ');
        compact.push_back(c);
        space = false;
    }
    return compact;
}

fs::path metadata_path(const std::string &module_path,
                       const std::string &version) {
    return fs::u8path(skein::modfile::native_build_dir(module_path, version)) /
           kMetadataName;
}

bool valid_artifact_name(const std::string &name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos &&
           name.find(':') == std::string::npos;
}

bool hash_file(const std::string &path, std::string &hash,
               std::string &error) {
    std::string data;
    if (!platform_read_file(path, data, error))
        return false;
    hash = skein_hex_encode(skein_sha256_bytes(data));
    return true;
}

bool load_metadata(const fs::path &path, std::map<std::string, std::string> &out,
                   std::string &error) {
    std::string data;
    if (!platform_read_file(path.u8string(), data, error))
        return false;
    std::size_t begin = 0;
    while (begin <= data.size()) {
        std::size_t end = data.find('\n', begin);
        std::string line = data.substr(begin, end == std::string::npos
                                                 ? std::string::npos
                                                 : end - begin);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::size_t equals = line.find('=');
        if (equals != std::string::npos) {
            const std::string key = trim_copy(line.substr(0, equals));
            const std::string value = trim_copy(line.substr(equals + 1));
            if (!key.empty())
                out[key] = value;
        }
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return true;
}

}  // namespace

std::string native_compiler_from_env() {
    std::string value;
    if (thread_env_get("SKEIN_CXX", value) && !value.empty())
        return value;
    if (thread_env_get("CXX", value) && !value.empty())
        return value;
    return "c++";
}

bool native_compiler_version(const std::string &compiler, std::string &version,
                             bool &available, std::string &error) {
    (void)error;
    version.clear();
    available = false;
    auto try_command = [&](const std::string &suffix,
                           bool require_success) -> bool {
        std::string output;
        int status = -1;
        if (!platform_run_command(shell_quote(compiler) + suffix, {}, output,
                                  status))
            return false;
        if (require_success && status != 0)
            return false;
        std::string line = first_line(output);
        if (line.empty())
            return false;
        version = std::move(line);
        return true;
    };
    if (try_command(" --version", true) || try_command(" -v", true) ||
        try_command("", false)) {
        available = true;
        return true;
    }
    return true;
}

bool write_native_artifact_metadata(const std::string &module_path,
                                    const std::string &version,
                                    const std::string &compiler,
                                    const std::string &compiler_version,
                                    const std::string &artifact,
                                    std::string &error) {
    std::string source_hash;
    if (!skein::modfile::dirhash(
            skein::modfile::cache_path(module_path, version), source_hash,
            error))
        return false;
    std::string artifact_hash;
    if (!hash_file(artifact, artifact_hash, error))
        return false;

    const fs::path artifact_path = fs::u8path(artifact);
    const std::string name = artifact_path.filename().u8string();
    if (!valid_artifact_name(name)) {
        error = "invalid native artifact name: " + name;
        return false;
    }

    const fs::path path = metadata_path(module_path, version);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "cannot create native metadata directory: " +
                path.parent_path().u8string();
        return false;
    }

    std::string data;
    data += "version=";
    data += kMetadataVersion;
    data += "\nsource_hash=";
    data += source_hash;
    data += "\ncompiler=";
    data += compiler;
    data += "\ncompiler_version=";
    data += compiler_version;
    data += "\nartifact_name=";
    data += name;
    data += "\nartifact_hash=";
    data += artifact_hash;
    data += "\n";
    return platform_write_file(path.u8string(), data, false, error);
}

bool validate_native_artifact(const std::string &module_path,
                              const std::string &version,
                              const std::string &output,
                              std::string &artifact, std::string &error) {
    const fs::path directory =
        fs::u8path(skein::modfile::native_build_dir(module_path, version));
    const fs::path metadata = metadata_path(module_path, version);
    std::error_code ec;
    if (!fs::is_regular_file(metadata, ec)) {
        error = "native artifact metadata is missing; run 'skein build " +
                module_path + "'";
        return false;
    }

    std::map<std::string, std::string> fields;
    if (!load_metadata(metadata, fields, error)) {
        error = "cannot read native artifact metadata: " + error;
        return false;
    }
    if (fields["version"] != kMetadataVersion) {
        error = "unsupported native artifact metadata version; run 'skein "
                "build " + module_path + "'";
        return false;
    }
    const std::string artifact_name = fields["artifact_name"];
    if (!valid_artifact_name(artifact_name)) {
        error = "invalid native artifact metadata; run 'skein build " +
                module_path + "'";
        return false;
    }
    if (!output.empty()) {
        const std::string expected = skein::modfile::native_library_name(output);
        if (artifact_name != expected) {
            error = "native artifact name changed; run 'skein build " +
                    module_path + "'";
            return false;
        }
    }

    const fs::path candidate = directory / artifact_name;
    if (!fs::is_regular_file(candidate, ec)) {
        error = "native artifact is missing; run 'skein build " + module_path +
                "'";
        return false;
    }

    std::string source_hash;
    if (!skein::modfile::dirhash(
            skein::modfile::cache_path(module_path, version), source_hash,
            error))
        return false;
    if (fields["source_hash"] != source_hash) {
        error = "native artifact is stale because module source changed; run "
                "'skein build " + module_path + "'";
        return false;
    }

    std::string artifact_hash;
    if (!hash_file(candidate.u8string(), artifact_hash, error))
        return false;
    if (fields["artifact_hash"] != artifact_hash) {
        error = "native artifact checksum mismatch; run 'skein build " +
                module_path + "'";
        return false;
    }

    /* Compiler version is checked when a compiler is available.  Deployed
     * applications may not have one, but they can still load verified bytes. */
    const std::string recorded_version = fields["compiler_version"];
    if (!recorded_version.empty() && recorded_version != "unknown") {
        std::string current_version;
        bool available = false;
        std::string ignored;
        native_compiler_version(native_compiler_from_env(), current_version,
                                available, ignored);
        if (available && current_version != recorded_version) {
            error = "native artifact compiler changed; run 'skein build " +
                    module_path + "'";
            return false;
        }
    }

    artifact = candidate.u8string();
    return true;
}
