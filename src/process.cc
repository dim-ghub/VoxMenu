#include "process.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

extern char** environ;

namespace nac {
namespace {

// Our environment without what loads Vox Menu, for helper programs.
std::vector<std::string> CleanEnvironment() {
  std::vector<std::string> env;
  for (char** e = environ; *e != nullptr; ++e) {
    const std::string var = *e;
    const std::string name = var.substr(0, var.find('='));
    if (name == "LD_PRELOAD" || name == "VK_ADD_IMPLICIT_LAYER_PATH" ||
        (name.size() > 6 && name.compare(name.size() - 6, 6, "_LAYER") == 0)) {
      continue;
    }
    env.push_back(var);
  }
  return env;
}

std::vector<char*> Pointers(std::vector<std::string>& strings) {
  std::vector<char*> out;
  for (std::string& s : strings) out.push_back(s.data());
  out.push_back(nullptr);
  return out;
}

// Runs a program and returns its exit status (-1 when it couldn't start),
// with its standard output in *out when given.
// Runs argv; with `started`, returns right away (pid in *started, its
// output readable from *stdout_fd), else waits and collects it into *out.
int RunImpl(std::vector<std::string> argv, std::string* out, pid_t* started, int* stdout_fd) {
  const bool piped = out != nullptr || stdout_fd != nullptr;
  int fds[2] = {-1, -1};
  if (piped && pipe2(fds, O_CLOEXEC) != 0) return -1;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (piped) posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  std::vector<std::string> env = CleanEnvironment();
  std::vector<char*> args = Pointers(argv), envp = Pointers(env);
  pid_t pid = -1;
  const int r = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), envp.data());
  posix_spawn_file_actions_destroy(&actions);
  if (piped) close(fds[1]);
  if (r != 0) {
    if (piped) close(fds[0]);
    return -1;
  }
  if (started != nullptr) {
    *started = pid;
    if (stdout_fd != nullptr) *stdout_fd = fds[0];
    return 0;
  }
  if (out != nullptr) {
    char buf[8192];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0 && out->size() < (4u << 20)) out->append(buf, n);
    close(fds[0]);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

int RunProgram(std::vector<std::string> argv, std::string* out) { return RunImpl(std::move(argv), out, nullptr, nullptr); }

pid_t StartProgram(std::vector<std::string> argv, int* stdout_fd) {
  pid_t pid = -1;
  if (RunImpl(std::move(argv), nullptr, &pid, stdout_fd) != 0) return -1;
  return pid;
}

}  // namespace nac
