// Running helper programs (curl, pactl, parec) from inside Nightcap,
// without passing on what loads Vox Menu into them.

#ifndef VOX_MENU_PROCESS_H_
#define VOX_MENU_PROCESS_H_

#include <sys/types.h>

#include <string>
#include <vector>

namespace nac {

// Runs a program (looked up on PATH) to completion. Returns its exit status,
// or -1 when it couldn't start; its standard output goes to *out when given.
int RunProgram(std::vector<std::string> argv, std::string* out);

// Starts a program and returns its pid (or -1), with *stdout_fd reading its
// standard output. The caller closes the fd and reaps the child.
pid_t StartProgram(std::vector<std::string> argv, int* stdout_fd);

}  // namespace nac

#endif  // VOX_MENU_PROCESS_H_
