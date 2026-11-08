// Roblox's own icon font, BuilderIcons, looked up by glyph name.
//
// The font ships inside the Roblox client Nightcap downloads
// (LuaPackages/.../BuilderIcons/Font/BuilderIcons-Regular.ttf). Its private
// codepoints may shift between Roblox versions, so icons are found by name
// ("gear", "chevron-large-left", ...) from the font's own tables.

#ifndef VOX_MENU_ICONS_H_
#define VOX_MENU_ICONS_H_

#include <cstdint>
#include <map>
#include <string>

namespace nac {

class IconFont {
 public:
  // Reads glyph names (post table) and codepoints (cmap) from a TrueType file.
  bool Load(const std::string& path);
  // The codepoint for a glyph name, or 0 if the font has no such icon.
  uint32_t Find(const std::string& name) const;
  // The same icon as UTF-8 text, or "" when missing.
  std::string Utf8(const std::string& name) const;
  const std::string& path() const { return path_; }

 private:
  std::map<std::string, uint32_t> codepoints_;
  std::string path_;
};

// The newest BuilderIcons-Regular.ttf among Nightcap's Roblox payloads, or "".
std::string FindRobloxIconFont();

}  // namespace nac

#endif  // VOX_MENU_ICONS_H_
