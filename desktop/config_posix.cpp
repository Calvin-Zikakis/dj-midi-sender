#include "config_posix.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace desktop {

namespace {

std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    return ".";
}

bool ensure_dir(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) return (st.st_mode & S_IFDIR);
    return ::mkdir(path.c_str(), 0700) == 0;
}

// Hand-rolled minimal JSON reader for our one-shape config. Tolerates extra
// whitespace and unknown keys; returns empty map on any parse error.
std::map<std::string, float> read_offsets(const std::string& path) {
    std::map<std::string, float> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    auto pos = text.find("\"clock_offsets_ms\"");
    if (pos == std::string::npos) return out;
    pos = text.find('{', pos);
    if (pos == std::string::npos) return out;
    ++pos;

    auto skip_ws = [&]() {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    };

    while (true) {
        skip_ws();
        if (pos >= text.size()) return {};
        if (text[pos] == '}') break;
        if (text[pos] != '"') return {};
        ++pos;
        std::string key;
        while (pos < text.size() && text[pos] != '"') {
            if (text[pos] == '\\' && pos + 1 < text.size()) {
                key += text[pos + 1];
                pos += 2;
            } else {
                key += text[pos++];
            }
        }
        if (pos >= text.size()) return {};
        ++pos;  // past closing "
        skip_ws();
        if (pos >= text.size() || text[pos] != ':') return {};
        ++pos;
        skip_ws();
        size_t num_start = pos;
        while (pos < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[pos])) ||
                text[pos] == '-' || text[pos] == '+' ||
                text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
        }
        std::string num = text.substr(num_start, pos - num_start);
        try {
            out[key] = std::stof(num);
        } catch (...) {
            return {};
        }
        skip_ws();
        if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
        if (pos < text.size() && text[pos] == '}') break;
        return {};
    }
    return out;
}

bool write_offsets(const std::string& path,
                   const std::map<std::string, float>& offsets) {
    std::ostringstream os;
    os << "{\n  \"clock_offsets_ms\": {";
    bool first = true;
    for (const auto& [key, val] : offsets) {
        os << (first ? "\n    " : ",\n    ");
        first = false;
        os << '"';
        for (char c : key) {
            if (c == '"' || c == '\\') os << '\\';
            os << c;
        }
        os << "\": " << val;
    }
    os << "\n  }\n}\n";

    std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    std::string data = os.str();
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok) { ::unlink(tmp.c_str()); return false; }
    return ::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

std::string config_path() {
    return home_dir() + "/.config/dj-midi-sender.json";
}

float load_clock_offset_ms(const std::string& port_name, float default_ms) {
    auto offsets = read_offsets(config_path());
    auto it = offsets.find(port_name);
    return (it == offsets.end()) ? default_ms : it->second;
}

bool save_clock_offset_ms(const std::string& port_name, float offset_ms) {
    if (!ensure_dir(home_dir() + "/.config")) return false;
    auto offsets = read_offsets(config_path());
    offsets[port_name] = offset_ms;
    return write_offsets(config_path(), offsets);
}

}  // namespace desktop
