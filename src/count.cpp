#include <chrono>
#include <iomanip>
#include <iostream>

#include <caf/all.hpp>
#include <caf/io/all.hpp>

using namespace caf;
using namespace caf::io;

namespace {

using ack_atom = caf::atom_constant<atom("ack")>;
using tag_atom = caf::atom_constant<atom("tag")>;
using done_atom = caf::atom_constant<atom("done")>;
using ping_atom = caf::atom_constant<atom("ping")>;
using pong_atom = caf::atom_constant<atom("pong")>;
using share_atom = caf::atom_constant<atom("share")>;
using measure_atom = caf::atom_constant<atom("measure")>;
using shutdown_atom = caf::atom_constant<atom("shutdown")>;

// -----------------------------------------------------------------------------
//  ACTOR SYSTEM CONFIG
// -----------------------------------------------------------------------------

class configuration : public actor_system_config {
public:
  std::string host = "localhost";
  std::string name = "";
  uint16_t port = 12345;
  uint16_t local_port = 0;
  uint16_t offset = 0;
  uint32_t others = 7;
  uint32_t timeout = 0;
  int rounds = 3;
  bool leader = false;
  configuration() {
    load<io::middleman>();
    opt_group{custom_options_,         "global"}
      .add(port,       "port,P",       "set remote port")
      .add(local_port, "local-port,L", "set local port")
      .add(host,       "host,H",       "set host")
      .add(offset,     "offset,O",     "set offset for ports (for repeated "
                                       "local testing)")
      .add(leader,     "leader,L",     "make this node the leader")
      .add(timeout,    "timeout,t",    "use a timeout (sec) instead of user "
                                       "input")
      .add(name,       "name,n",       "name used for debugging")
      .add(rounds,     "rounds,r",     "number of measurement rounds")
      .add(others,     "others,o",     "set number of other nodes");
  }
};

struct cache {
  actor next;
  std::unordered_map<std::string, actor> others;
  std::unordered_map<std::string, std::set<int>> answers;
};

behavior ping_test(stateful_actor<cache>* self, const std::string& my_name,
                   int rounds, actor main_actor) {
  self->set_default_handler(skip);
  return {
    [=](actor next) {
      aout(self) << "[n] " << next.node().process_id() << std::endl;
      self->state.next = next;
      self->send(next, share_atom::value, self, my_name);
      self->set_default_handler(print_and_drop);
      self->become(
        [=](share_atom, actor other, const std::string& name) {
          auto& s = self->state;
          if (other == self) {
            aout(self) << "[r] actor returned" << std::endl;
            self->send(main_actor, done_atom::value);
          } else {
            s.others[name] = other;
            aout(self) << "[s] " << name << std::endl;
            self->send(self->state.next, share_atom::value, other, name);
          }
        },
        [=](measure_atom, int round) {
          if (round > rounds) {
            self->send(main_actor, done_atom::value);
          } else {
            for (auto& o : self->state.others)
              self->send(o.second, ping_atom::value, round, my_name);
            self->delayed_send(self, std::chrono::milliseconds(100),
                               measure_atom::value, round + 1);
          }
        },
        [=](ping_atom, int round, const std::string& name) {
          aout(self) << "[i] " << name << std::endl;
          return make_message(pong_atom::value, round, my_name);
        },
        [=](pong_atom, int round, const std::string& name) {
          aout(self) << "[o] " << name << std::endl;
          self->state.answers[name].insert(round);
        },
        [=](shutdown_atom) {
          for (auto& o : self->state.others) {
            std::set<int> missing;
            for (int i = 0; i < rounds; ++i)
              if (self->state.answers[o.first].count(i) == 0)
                missing.insert(i);
            aout(self) << o.first << " failed to answer to " << missing.size()
                       << " pings" << std::endl;
          }
          aout(self) << "shutdown!" << std::endl;
          self->quit();
          self->send(main_actor, done_atom::value);
        }
      );
    }
  };
}

} // namespace anonymous


struct net_stuff {
  net_stuff(actor_system& sys, const configuration& config)
    : sys(sys), config(config) {
    // nop
  }

  template <class ...Ts>
  auto remote_actor(Ts&&... args) {
    if (config.middleman_enable_udp)
      return sys.middleman().remote_actor_udp(std::forward<Ts>(args)...);
    else
      return sys.middleman().remote_actor(std::forward<Ts>(args)...);
  }

  template <class ...Ts>
  auto publish(Ts&&... args) {
    if (config.middleman_enable_udp)
      return sys.middleman().publish_udp(std::forward<Ts>(args)...);
    else
      return sys.middleman().publish(std::forward<Ts>(args)...);
  }

  actor_system& sys;
  const configuration& config;
};

void caf_main(actor_system& system, const configuration& config) {
  scoped_actor self{system};
  auto catch_up = [&]() {
    if (config.timeout > 0) {
      aout(self) << "Waiting for " << config.timeout << " seconds to give all "
                   "nodes a chance to catch up" << std::endl;
      self->delayed_send(self, std::chrono::seconds(config.timeout),
                         ping_atom::value);
      self->receive([&](ping_atom) {
        aout(self) << std::endl << "let's continue" << std::endl;
      });
    } else {
      aout(self) << "Press any key to continue ... " << std::endl;
      std::cin.get();
      aout(self) << std::endl << "let's continue" << std::endl;
    }
  };
  aout(self) << "Config: \n > host = " << config.host << std::endl
            << " > port = " << config.port << std::endl
            << " > local-port = " << config.local_port << std::endl
            << " > others = " << config.others << std::endl
            << " > offset = " << config.offset << std::endl
            << " > leader = " << (config.leader ? "y" : "n") << std::endl
            << " > udp = " << (config.middleman_enable_udp ? "y" : "n")
            << std::endl
            << " > tcp = " << (config.middleman_enable_tcp ? "y" : "n")
            << std::endl
            << " > timeout = " << config.timeout << std::endl
            << " > rounds = " << config.rounds << std::endl
            << " > name = " << config.name << std::endl
            << " > id = " << system.node().process_id() << std::endl;;
  net_stuff ns(system, config);
  auto remote_port = config.port + config.offset;
  auto local_port = config.local_port + config.offset;
  auto name = config.name.empty() ? std::to_string(system.node().process_id())
                                  : config.name;
  if (config.local_port == 0)
    local_port = remote_port;
  auto pt = system.spawn(ping_test, name, config.rounds, self);
  aout(self) << std::endl << "Opening local port ... " << std::endl;
  auto port = ns.publish(pt, local_port, nullptr, true);
  if (!port) {
    std::cerr << "Could not publish my actor on port " << local_port
              << std::endl;
    return;
  }
  aout(self) << "Published actor on " << *port << std::endl;
  // Wait for user input. Make sure all participants published their actor.
  catch_up();
  auto next = ns.remote_actor(config.host, remote_port);
  if (!next) {
    std::cerr << "Could not connect to next node! (" << config.host << ":"
              << remote_port << ")" << std::endl;
    return;
  }
  aout(self) << "Connected." << std::endl << std::endl
            << "Starting interaction ..." << std::endl;
  self->send(pt, *next);
  self->receive(
    [&](done_atom) {
      aout(self) << "shared actor with all others" << std::endl;
    }
  );
  catch_up();
  self->send(pt, measure_atom::value, 0);
  self->receive(
    [&](done_atom) {
      aout(self) << "performed all measurements" << std::endl;
    }
  );
  catch_up();
  self->send(pt, shutdown_atom::value);
  catch_up();
  aout(self) << "bye" << std::endl;
}

CAF_MAIN();
