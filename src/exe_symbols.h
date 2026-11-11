// Finding data objects in Nightcap's own executable by symbol name, from
// the static symbol table in /proc/self/exe. Nightcap ships unstripped; on a
// stripped build nothing matches and callers fall back.

#ifndef VOX_MENU_EXE_SYMBOLS_H_
#define VOX_MENU_EXE_SYMBOLS_H_

#include <cstddef>
#include <functional>
#include <string_view>

namespace nac {

// Calls `match` with each data object's mangled name and size; it returns
// which of the `count` wanted objects that is (or -1). Fills out[i] with the
// runtime address of each, and returns true when every one was found.
bool FindExeObjects(const std::function<int(std::string_view name, std::size_t size)>& match, void** out,
                    int count);

}  // namespace nac

#endif  // VOX_MENU_EXE_SYMBOLS_H_
