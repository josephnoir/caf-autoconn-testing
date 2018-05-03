#include <chrono>
#include <iostream>

#include <caf/all.hpp>
#include <caf/io/all.hpp>

using namespace caf;
using namespace caf::io;

namespace {

using ping_atom = caf::atom_constant<atom("ping")>;
using share_atom = caf::atom_constant<atom("share")>;

using std::chrono::system_clock;
using std::chrono::milliseconds;
using std::chrono::duration_cast;

constexpr auto delay = std::chrono::seconds(1);

// -----------------------------------------------------------------------------
//  ACTOR SYSTEM CONFIG
// -----------------------------------------------------------------------------

class configuration : public actor_system_config {
public:
  std::string host = "localhost";
  uint16_t port = 12345;
  uint16_t local_port = 0;
  uint32_t others = 7;
  configuration() {
    load<io::middleman>();
    set("middleman.enable-tcp", true);
    set("middleman.enable-udp", false);
    opt_group{custom_options_, "global"}
      .add(port, "port,P", "set remote port")
      .add(port, "local-port,L", "set local port")
      .add(host, "host,H", "set host")
      .add(others, "others,o", "set number of other nodes");
  }
};

struct cache {
  actor next;
  uint32_t received_pings;
  uint32_t sent_pings;
  system_clock::time_point start;
};

behavior ping_test(stateful_actor<cache>* self, uint32_t other_nodes) {
  self->state.received_pings = 0;
  self->state.sent_pings = 0;
  return {
    [=](actor next) {
      std::cout << "[+] " << to_string(next.node()) << std::endl;
      self->state.start = system_clock::now();
      self->state.next = next;
      self->send(next, share_atom::value, self);
    },
    [=](share_atom, actor an_actor) {
      auto&s = self->state;
      if (an_actor == self) {
        std::cout << "[!] actor returned" << std::endl;
        auto dur = system_clock::now() - s.start;
        std::cout << "Got my actor back after "
                  << duration_cast<milliseconds>(dur).count()
                  << std::endl;
      } else {
        std::cout << "[+] " << to_string(an_actor.node()) << std::endl;
        self->send(s.next, share_atom::value, an_actor);
        self->send(an_actor, ping_atom::value);
        s.sent_pings += 1;
        if (s.sent_pings >= other_nodes && s.received_pings >= other_nodes) {
          auto dur = system_clock::now() - self->state.start;
          std::cout << "Quitting after "
                    << duration_cast<milliseconds>(dur).count()
                    << std::endl;
          self->quit();
        }
      }
    },
    [=](ping_atom) {
      std::cout << "[>] " << to_string(self->current_sender()->node()) << std::endl;
      auto&s = self->state;
      s.received_pings += 1;
      if (s.received_pings >= other_nodes && s.sent_pings >= other_nodes) {
        auto dur = system_clock::now() - self->state.start;
        std::cout << "Quitting after "
                  << duration_cast<milliseconds>(dur).count()
                  << std::endl;
        self->quit();
      }
    }
  };
}

} // namespace anonymous

void caf_main(actor_system& system, const configuration& config) {
  auto remote_port = config.port;
  auto local_port = config.local_port;
  if (local_port == 0)
    local_port = remote_port;
  std::cout << "System on node " << to_string(system.node()) << std::endl;
  scoped_actor self{system};
  auto pt = system.spawn(ping_test, config.others);
  auto port = system.middleman().publish(pt, local_port);
  if (!port) {
    std::cerr << "Could not publish my actor on port " << local_port
              << std::endl;
    return;
  }
  self->delayed_send(self, delay, ping_atom::value);
  self->receive([&](ping_atom) { std::cout << "Let's do this!" << std::endl; });
  auto next = system.middleman().remote_actor(config.host, remote_port);
  if (!next) {
    std::cerr << "Could not connect to next node! (" << config.host << ":"
              << remote_port << ")" << std::endl;
    return;
  }
  self->send(pt, *next);
}

CAF_MAIN();
