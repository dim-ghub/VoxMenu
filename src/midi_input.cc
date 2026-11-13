#include "midi_input.h"

#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <poll.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include "app.h"
#include "midi_file.h"

namespace nac::midi_input {
namespace {

// The few libasound functions used, looked up at run time so the library
// doesn't need ALSA to load.
struct Alsa {
#define ALSA_FN(name) decltype(&::name) name = nullptr;
  ALSA_FN(snd_seq_open)
  ALSA_FN(snd_seq_close)
  ALSA_FN(snd_seq_set_client_name)
  ALSA_FN(snd_seq_client_id)
  ALSA_FN(snd_seq_create_simple_port)
  ALSA_FN(snd_seq_connect_from)
  ALSA_FN(snd_seq_event_input)
  ALSA_FN(snd_seq_poll_descriptors_count)
  ALSA_FN(snd_seq_poll_descriptors)
  ALSA_FN(snd_seq_client_info_malloc)
  ALSA_FN(snd_seq_client_info_free)
  ALSA_FN(snd_seq_client_info_set_client)
  ALSA_FN(snd_seq_client_info_get_client)
  ALSA_FN(snd_seq_client_info_get_name)
  ALSA_FN(snd_seq_query_next_client)
  ALSA_FN(snd_seq_port_info_malloc)
  ALSA_FN(snd_seq_port_info_free)
  ALSA_FN(snd_seq_port_info_set_client)
  ALSA_FN(snd_seq_port_info_set_port)
  ALSA_FN(snd_seq_port_info_get_port)
  ALSA_FN(snd_seq_port_info_get_capability)
  ALSA_FN(snd_seq_query_next_port)
#undef ALSA_FN
};

const Alsa* LoadAlsa() {
  static const Alsa* alsa = [] () -> const Alsa* {
    void* lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) return nullptr;
    static Alsa a;
    bool ok = true;
#define ALSA_FN(name) ok = ok && (a.name = reinterpret_cast<decltype(a.name)>(dlsym(lib, #name))) != nullptr;
    ALSA_FN(snd_seq_open)
    ALSA_FN(snd_seq_close)
    ALSA_FN(snd_seq_set_client_name)
    ALSA_FN(snd_seq_client_id)
    ALSA_FN(snd_seq_create_simple_port)
    ALSA_FN(snd_seq_connect_from)
    ALSA_FN(snd_seq_event_input)
    ALSA_FN(snd_seq_poll_descriptors_count)
    ALSA_FN(snd_seq_poll_descriptors)
    ALSA_FN(snd_seq_client_info_malloc)
    ALSA_FN(snd_seq_client_info_free)
    ALSA_FN(snd_seq_client_info_set_client)
    ALSA_FN(snd_seq_client_info_get_client)
    ALSA_FN(snd_seq_client_info_get_name)
    ALSA_FN(snd_seq_query_next_client)
    ALSA_FN(snd_seq_port_info_malloc)
    ALSA_FN(snd_seq_port_info_free)
    ALSA_FN(snd_seq_port_info_set_client)
    ALSA_FN(snd_seq_port_info_set_port)
    ALSA_FN(snd_seq_port_info_get_port)
    ALSA_FN(snd_seq_port_info_get_capability)
    ALSA_FN(snd_seq_query_next_port)
#undef ALSA_FN
    return ok ? &a : nullptr;
  }();
  return alsa;
}

std::mutex g_mutex;
std::deque<Message> g_queue;
std::vector<std::string> g_devices;
std::string g_error;
// Never destroyed: a joinable std::thread left at exit would abort.
std::thread* g_thread = nullptr;
std::atomic<bool> g_run{false};
std::atomic<bool> g_running{false};

void SetError(const std::string& error) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_error = error;
}

// Subscribes to every readable port not connected yet; returns the names
// of the clients listened to.
std::vector<std::string> ConnectAll(const Alsa& a, snd_seq_t* seq, int self, int port,
                                    std::set<std::pair<int, int>>* connected) {
  std::vector<std::string> names;
  snd_seq_client_info_t* ci;
  snd_seq_port_info_t* pi;
  a.snd_seq_client_info_malloc(&ci);
  a.snd_seq_port_info_malloc(&pi);
  a.snd_seq_client_info_set_client(ci, -1);
  while (a.snd_seq_query_next_client(seq, ci) >= 0) {
    const int client = a.snd_seq_client_info_get_client(ci);
    const std::string name = a.snd_seq_client_info_get_name(ci);
    // The system client and "Midi Through" carry nothing played.
    if (client == 0 || client == self || name == "Midi Through") continue;
    bool any = false;
    a.snd_seq_port_info_set_client(pi, client);
    a.snd_seq_port_info_set_port(pi, -1);
    while (a.snd_seq_query_next_port(seq, pi) >= 0) {
      const unsigned caps = a.snd_seq_port_info_get_capability(pi);
      if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) !=
          (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) {
        continue;
      }
      const int p = a.snd_seq_port_info_get_port(pi);
      if (connected->count({client, p}) || a.snd_seq_connect_from(seq, port, client, p) >= 0) {
        connected->insert({client, p});
        any = true;
      }
    }
    if (any) names.push_back(name);
  }
  a.snd_seq_port_info_free(pi);
  a.snd_seq_client_info_free(ci);
  return names;
}

void Listen() {
  const Alsa* a = LoadAlsa();
  if (a == nullptr) {
    SetError("ALSA (libasound) isn't installed");
    g_run = false;
    return;
  }
  snd_seq_t* seq = nullptr;
  if (a->snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
    SetError("Can't open the ALSA sequencer");
    g_run = false;
    return;
  }
  a->snd_seq_set_client_name(seq, "Vox Menu");
  const int port = a->snd_seq_create_simple_port(seq, "Piano", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                                 SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  const int self = a->snd_seq_client_id(seq);
  SetError("");
  g_running = true;
  Log("vox-menu: listening for MIDI keyboards");

  std::set<std::pair<int, int>> connected;
  auto last_scan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  std::vector<pollfd> fds(std::max(1, a->snd_seq_poll_descriptors_count(seq, POLLIN)));
  a->snd_seq_poll_descriptors(seq, fds.data(), fds.size(), POLLIN);
  while (g_run) {
    // Keyboards plugged in later are picked up within a couple of seconds.
    const auto now = std::chrono::steady_clock::now();
    if (now - last_scan > std::chrono::seconds(2)) {
      last_scan = now;
      std::vector<std::string> names = ConnectAll(*a, seq, self, port, &connected);
      std::lock_guard<std::mutex> lock(g_mutex);
      g_devices = std::move(names);
    }
    if (poll(fds.data(), fds.size(), 200) <= 0) continue;
    snd_seq_event_t* ev = nullptr;
    while (a->snd_seq_event_input(seq, &ev) >= 0 && ev != nullptr) {
      Message m;
      if (ev->type == SND_SEQ_EVENT_NOTEON || ev->type == SND_SEQ_EVENT_NOTEOFF) {
        m.note = ev->data.note.note;
        m.velocity = ev->data.note.velocity;
        m.down = ev->type == SND_SEQ_EVENT_NOTEON && m.velocity > 0;
      } else if (ev->type == SND_SEQ_EVENT_CONTROLLER && ev->data.control.param == 64) {
        m.note = midi::kSustain;
        m.velocity = ev->data.control.value;
        m.down = m.velocity >= 64;
      } else if (ev->type == SND_SEQ_EVENT_PORT_UNSUBSCRIBED) {
        // Unplugged: forget it, so it's connected again if it comes back.
        connected.erase({ev->data.connect.sender.client, ev->data.connect.sender.port});
        continue;
      } else {
        continue;
      }
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_queue.size() < 1024) g_queue.push_back(m);
    }
  }
  a->snd_seq_close(seq);
  g_running = false;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_devices.clear();
  g_queue.clear();
}

void Join() {
  if (g_thread == nullptr) return;
  g_thread->join();
  delete g_thread;
  g_thread = nullptr;
}

}  // namespace

void Start() {
  if (g_run.exchange(true)) return;
  Join();  // one that gave up
  g_thread = new std::thread(Listen);
}

void Stop() {
  g_run = false;
  Join();
}

bool Running() { return g_running; }

std::vector<std::string> Devices() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_devices;
}

std::string Error() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_error;
}

bool Poll(Message* message) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_queue.empty()) return false;
  *message = g_queue.front();
  g_queue.pop_front();
  return true;
}

}  // namespace nac::midi_input
