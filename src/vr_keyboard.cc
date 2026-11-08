#include "vr_keyboard.h"

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstring>

#include "app.h"

namespace nac {
namespace {

// Mirrors mocktail::vr::VirtualKeyboardHost (include/mocktail/vr/
// virtual_keyboard.h in Nightcap). The symbol's size is checked against it.
struct Host {
  void* context;
  bool (*input_requested)(void* context);
  void (*commit_text)(void* context, const char* utf8);
  void (*press_key)(void* context, std::uint32_t scancode, bool ctrl);
};

// mocktail::vr::(anonymous namespace)::g_host and g_host_mutex. Matched by
// suffix so either compiler's mangling of the anonymous namespace works.
constexpr const char kPrefix[] = "_ZN8mocktail2vr";
constexpr const char kHostSuffix[] = "6g_hostE";
constexpr const char kMutexSuffix[] = "12g_host_mutexE";

enum class State { kUnresolved, kFound, kMissing };
State g_state = State::kUnresolved;  // main thread only
Host* g_host = nullptr;
pthread_mutex_t* g_host_mutex = nullptr;

VrKeyboardObserver g_observer;  // set before the wrappers are installed
std::atomic<void*> g_context{nullptr};
std::atomic<void (*)(void*, const char*)> g_real_commit{nullptr};
std::atomic<void (*)(void*, std::uint32_t, bool)> g_real_press{nullptr};

bool EndsWith(const char* s, size_t len, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return len >= n && std::memcmp(s + len - n, suffix, n) == 0;
}

// The main program's load bias (0 for a non-PIE executable).
int FirstObject(struct dl_phdr_info* info, size_t, void* out) {
  *static_cast<ElfW(Addr)*>(out) = info->dlpi_addr;
  return 1;  // the first entry is the executable
}

// Reads the executable's static symbol table. Nightcap ships unstripped,
// but a stripped build simply has no match.
bool FindSymbols() {
  const int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st {};
  void* map = MAP_FAILED;
  if (fstat(fd, &st) == 0 && st.st_size > static_cast<off_t>(sizeof(Elf64_Ehdr))) {
    map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  }
  close(fd);
  if (map == MAP_FAILED) return false;

  const auto* base = static_cast<const unsigned char*>(map);
  const size_t size = st.st_size;
  ElfW(Addr) host_addr = 0, mutex_addr = 0;
  const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
  const bool elf = std::memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 &&
                   eh->e_ident[EI_CLASS] == ELFCLASS64 &&
                   eh->e_shentsize == sizeof(Elf64_Shdr) && eh->e_shoff < size &&
                   eh->e_shnum <= (size - eh->e_shoff) / sizeof(Elf64_Shdr);
  if (elf) {
    const auto* sh = reinterpret_cast<const Elf64_Shdr*>(base + eh->e_shoff);
    for (unsigned i = 0; i < eh->e_shnum; ++i) {
      if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh->e_shnum) continue;
      const Elf64_Shdr& strs = sh[sh[i].sh_link];
      if (sh[i].sh_offset > size || sh[i].sh_size > size - sh[i].sh_offset ||
          strs.sh_offset > size || strs.sh_size > size - strs.sh_offset) {
        continue;
      }
      const auto* syms = reinterpret_cast<const Elf64_Sym*>(base + sh[i].sh_offset);
      const size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
      const char* names = reinterpret_cast<const char*>(base + strs.sh_offset);
      for (size_t j = 0; j < count; ++j) {
        const Elf64_Sym& sym = syms[j];
        if (ELF64_ST_TYPE(sym.st_info) != STT_OBJECT || sym.st_name >= strs.sh_size) continue;
        const char* name = names + sym.st_name;
        const size_t len = strnlen(name, strs.sh_size - sym.st_name);
        if (len < sizeof(kPrefix) - 1 || std::memcmp(name, kPrefix, sizeof(kPrefix) - 1) != 0) {
          continue;
        }
        if (EndsWith(name, len, kHostSuffix) && sym.st_size == sizeof(Host)) {
          host_addr = sym.st_value;
        } else if (EndsWith(name, len, kMutexSuffix) && sym.st_size == sizeof(pthread_mutex_t)) {
          mutex_addr = sym.st_value;
        }
      }
    }
  }
  munmap(map, size);
  if (host_addr == 0 || mutex_addr == 0) return false;

  ElfW(Addr) bias = 0;
  dl_iterate_phdr(FirstObject, &bias);
  g_host = reinterpret_cast<Host*>(bias + host_addr);
  g_host_mutex = reinterpret_cast<pthread_mutex_t*>(bias + mutex_addr);
  return true;
}

void CommitText(void* context, const char* utf8) {
  if (utf8 != nullptr && g_observer.text != nullptr) g_observer.text(utf8);
  if (auto real = g_real_commit.load()) real(context, utf8);
}

void PressKey(void* context, std::uint32_t scancode, bool ctrl) {
  if (g_observer.key != nullptr && g_observer.key(scancode, ctrl)) return;
  if (auto real = g_real_press.load()) real(context, scancode, ctrl);
}

}  // namespace

void HookVrKeyboard(const VrKeyboardObserver& observer) {
  if (g_state == State::kMissing) return;
  if (g_state == State::kUnresolved) {
    if (!FindSymbols()) {
      g_state = State::kMissing;
      Log("VR keyboard not found in Nightcap (stripped or non-VR build); "
          "chat typed in a headset won't be spoken");
      return;
    }
    g_state = State::kFound;
    g_observer = observer;
  }
  // Nightcap installs its host once VR starts, and could replace it; wrap
  // whatever is there now. The render thread copies it under this mutex.
  pthread_mutex_lock(g_host_mutex);
  bool wrapped = false;
  if (g_host->commit_text != nullptr && g_host->commit_text != CommitText) {
    g_real_commit.store(g_host->commit_text);
    g_host->commit_text = CommitText;
    wrapped = true;
  }
  if (g_host->press_key != nullptr && g_host->press_key != PressKey) {
    g_real_press.store(g_host->press_key);
    g_host->press_key = PressKey;
    wrapped = true;
  }
  if (wrapped) g_context.store(g_host->context);
  pthread_mutex_unlock(g_host_mutex);
  if (wrapped) Log("following the VR keyboard for chat");
}

bool VrPressKey(std::uint32_t scancode, bool ctrl) {
  auto real = g_real_press.load();
  if (real == nullptr) return false;
  real(g_context.load(), scancode, ctrl);
  return true;
}

}  // namespace nac
