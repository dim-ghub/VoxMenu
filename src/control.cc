// A datagram socket the launcher uses to reach a running Nightcap:
// $XDG_RUNTIME_DIR/vox-menu/control-<pid>.sock.
// Sending "open" opens the in-game menu (when Vox Menu is launched while
// Nightcap is already running).

#include "control.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "app.h"
#include "overlay.h"

namespace nac {
namespace {

std::once_flag g_started;

void Serve() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime == nullptr || *runtime == 0) return;
  const std::string dir = std::string(runtime) + "/vox-menu";
  mkdir(dir.c_str(), 0700);
  const std::string path = dir + "/control-" + std::to_string(getpid()) + ".sock";
  sockaddr_un addr{};
  if (path.size() >= sizeof(addr.sun_path)) return;
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return;
  unlink(path.c_str());
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return;
  }
  chmod(path.c_str(), 0600);

  std::atexit([] {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    if (rt) {
      unlink((std::string(rt) + "/vox-menu/control-" + std::to_string(getpid()) + ".sock").c_str());
    }
  });
  char buf[64];
  while (true) {
    const ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) continue;
    buf[n] = 0;
    if (std::strncmp(buf, "open", 4) == 0) overlay::RequestOpen();
  }
}

}  // namespace

void StartControl() {
  std::call_once(g_started, [] { std::thread(Serve).detach(); });
}

}  // namespace nac
