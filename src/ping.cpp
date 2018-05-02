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
  uint32_t others = 7;
  configuration() {
    load<io::middleman>();
    set("middleman.enable-tcp", true);
    set("middleman.enable-udp", false);
    opt_group{custom_options_, "global"}
      .add(port, "port,P", "set port")
      .add(host, "host,H", "set host")
      .add(others, "others,o", "set number of other nodes");
  }
};

struct cache {
  actor next;
  uint32_t received_pings;
  system_clock::time_point start;
};

behavior ping_test(stateful_actor<cache>* self, uint32_t expected_pings) {
  self->state.received_pings = 0;
  return {
    [=](actor next) {
      self->state.start = system_clock::now();
      self->state.next = next;
      self->send(next, share_atom::value, self);
    },
    [=](share_atom, actor an_actor) {
      if (an_actor == self) {
        auto dur = system_clock::now() - self->state.start;
        std::cout << "Got my actor back after "
                  << duration_cast<milliseconds>(dur).count()
                  << std::endl;
      } else {
        std::cout << "Sending message to node " << to_string(an_actor.node())
                  << std::endl;
        self->send(self->state.next, share_atom::value, an_actor);
        self->send(an_actor, ping_atom::value);
      }
    },
    [=](ping_atom) {
      auto&s = self->state;
      s.received_pings += 1;
      if (s.received_pings >= expected_pings) {
        auto dur = system_clock::now() - self->state.start;
        std::cout << "Got all pings after "
                  << duration_cast<milliseconds>(dur).count()
                  << std::endl;
        self->quit();
      }
    }
  };
}

} // namespace anonymous

void caf_main(actor_system& system, const configuration& config) {
  std::cout << "System on node " << to_string(system.node()) << std::endl;
  scoped_actor self{system};
  auto pt = system.spawn(ping_test, config.others);
  auto port = system.middleman().publish(pt, config.port);
  if (!port) {
    std::cerr << "Could not publish my actor on port " << config.port
              << std::endl;
    return;
  }
  self->delayed_send(self, delay, ping_atom::value);
  self->receive([&](ping_atom) { std::cout << "Let's do this!" << std::endl; });
  auto next = system.middleman().remote_actor(config.host, config.port);
  if (!next) {
    std::cerr << "Could not connect to next node! (" << config.host << ":"
              << config.port << ")" << std::endl;
    return;
  }
  self->send(pt, *next);
}

CAF_MAIN();
