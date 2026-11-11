#include "exe_symbols.h"

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace nac {
namespace {

// The main program's load bias (0 for a non-PIE executable).
int FirstObject(struct dl_phdr_info* info, size_t, void* out) {
  *static_cast<ElfW(Addr)*>(out) = info->dlpi_addr;
  return 1;  // the first entry is the executable
}

}  // namespace

bool FindExeObjects(const std::function<int(std::string_view name, std::size_t size)>& match, void** out,
                    int count) {
  for (int i = 0; i < count; ++i) out[i] = nullptr;
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
  std::vector<ElfW(Addr)> found(count, 0);
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
      const size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
      const char* names = reinterpret_cast<const char*>(base + strs.sh_offset);
      for (size_t j = 0; j < n; ++j) {
        const Elf64_Sym& sym = syms[j];
        if (ELF64_ST_TYPE(sym.st_info) != STT_OBJECT || sym.st_name >= strs.sh_size) continue;
        const char* name = names + sym.st_name;
        const int slot = match(std::string_view(name, strnlen(name, strs.sh_size - sym.st_name)), sym.st_size);
        if (slot >= 0 && slot < count) found[slot] = sym.st_value;
      }
    }
  }
  munmap(map, size);

  ElfW(Addr) bias = 0;
  dl_iterate_phdr(FirstObject, &bias);
  bool all = true;
  for (int i = 0; i < count; ++i) {
    if (found[i] == 0) {
      all = false;
      continue;
    }
    out[i] = reinterpret_cast<void*>(bias + found[i]);
  }
  return all;
}

}  // namespace nac
