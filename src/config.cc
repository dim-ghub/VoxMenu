#include "config.h"

#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace nac {
namespace {

// Just enough JSON for a flat object of strings, numbers and booleans.
std::map<std::string, std::string> ParseFlatJson(const std::string& text) {
  std::map<std::string, std::string> values;
  size_t i = 0;
  auto skip = [&]() {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
  };
  auto read_string = [&](std::string* out) {
    if (i >= text.size() || text[i] != '"') return false;
    ++i;
    while (i < text.size() && text[i] != '"') {
      if (text[i] == '\\' && i + 1 < text.size()) {
        ++i;
        switch (text[i]) {
          case 'n': *out += '\n'; break;
          case 't': *out += '\t'; break;
          default: *out += text[i];
        }
      } else {
        *out += text[i];
      }
      ++i;
    }
    ++i;
    return true;
  };
  skip();
  if (i >= text.size() || text[i] != '{') return values;
  ++i;
  while (true) {
    skip();
    std::string key;
    if (!read_string(&key)) break;
    skip();
    if (i >= text.size() || text[i] != ':') break;
    ++i;
    skip();
    std::string value;
    if (i < text.size() && text[i] == '"') {
      read_string(&value);
      value = "\"" + value;  // mark strings
    } else {
      while (i < text.size() && text[i] != ',' && text[i] != '}') value += text[i++];
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    }
    values[key] = value;
    skip();
    if (i < text.size() && text[i] == ',') {
      ++i;
      continue;
    }
    break;
  }
  return values;
}

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out += c;
  }
  return out + "\"";
}

bool WriteFileAtomic(const std::string& path, const std::string& content) {
  const std::string dir = path.substr(0, path.rfind('/'));
  mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755);
  mkdir(dir.c_str(), 0755);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp);
    if (!out) return false;
    out << content;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string ConfigBase() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  return xdg && *xdg ? xdg : std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.config";
}

// Older names of this project; their settings are picked up once.
const char* const kLegacyDirs[] = {"vox-tts", "nightcap-ac"};

std::string Number(double d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", d);
  return buf;
}

}  // namespace

std::string Config::Path() { return ConfigBase() + "/vox-menu/config.json"; }

std::string Config::ToJson() const {
  std::string json = "{\n";
  bool first = true;
  auto add = [&](const char* key, const std::string& value) {
    if (!first) json += ",\n";
    first = false;
    json += "  " + Quote(key) + ": " + value;
  };
  struct Writer {
    decltype(add)& out;
    void operator()(const char* k, const bool& b) { out(k, b ? "true" : "false"); }
    void operator()(const char* k, const double& d) { out(k, Number(d)); }
    void operator()(const char* k, const std::string& s) { out(k, Quote(s)); }
  };
  const_cast<Config*>(this)->Visit(Writer{add});
  return json + "\n}\n";
}

Config Config::Load() {
  Config c;
  std::ifstream file(Path());
  bool migrated = false;
  for (const char* legacy : kLegacyDirs) {
    if (file) break;
    file.open(ConfigBase() + "/" + legacy + "/config.json");
    migrated = static_cast<bool>(file);
  }
  if (!file) return c;
  std::stringstream ss;
  ss << file.rdbuf();
  const auto v = ParseFlatJson(ss.str());
  struct Reader {
    const std::map<std::string, std::string>& v;
    const std::string* Find(const char* k) {
      auto it = v.find(k);
      return it == v.end() || it->second.empty() ? nullptr : &it->second;
    }
    void operator()(const char* k, bool& b) {
      if (const std::string* s = Find(k)) b = *s == "true";
    }
    void operator()(const char* k, double& d) {
      if (const std::string* s = Find(k); s && (*s)[0] != '"') d = std::atof(s->c_str());
    }
    void operator()(const char* k, std::string& out) {
      if (const std::string* s = Find(k); s && (*s)[0] == '"') out = s->substr(1);
    }
  };
  c.Visit(Reader{v});
  if (!c.uses_vc() && !c.uses_local()) c.output = "vc";
  if (migrated) c.Save();
  return c;
}

bool Config::Save() const {
  const std::string json = ToJson();
  return WriteFileAtomic(Path(), json);
}

}  // namespace nac
