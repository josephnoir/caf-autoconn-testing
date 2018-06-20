#include <chrono>
#include <iomanip>
#include <iostream>

#include <caf/all.hpp>
#include <caf/io/all.hpp>

using namespace caf;
using namespace caf::io;

namespace {

using ack_atom = caf::atom_constant<atom("ack")>;
using done_atom = caf::atom_constant<atom("done")>;
using ping_atom = caf::atom_constant<atom("ping")>;
using pong_atom = caf::atom_constant<atom("pong")>;
using peer_atom = caf::atom_constant<atom("peer")>;
using share_atom = caf::atom_constant<atom("share")>;
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
  int retransmits = 3;
  bool leader = false;
  configuration() {
    load<io::middleman>();
    opt_group{custom_options_,         "global"}
      .add(port,       "port,P",       "set remote port")
      .add(local_port, "local-port,L", "set local port")
      .add(host,       "host,H",       "set host")
      .add(offset,     "offset,O",     "set offset for ports (for repeated local testing)")
      .add(leader,     "leader,L",     "make this node the leader")
      .add(timeout,    "timeout,t",    "use a timeout (sec) instead of user input")
      .add(name,       "name,n",       "name used for debugging")
      .add(retransmits,"retransmits,r","maxmimum number of retransmits")
      .add(others,     "others,o",     "set number of other nodes");
  }
};

struct cache {
  actor leader;
  actor next;
  uint32_t received_pongs;
  bool received_done;
  std::unordered_map<actor, uint32_t> sending;
  std::unordered_map<strong_actor_ptr, std::set<uint32_t>> receiving;
};

template <class ... Ts>
void send_reliably(stateful_actor<cache>* self, const actor dest,
                   int max_retransmits, Ts ... xs) {
  auto sequence_number = self->state.sending[dest]++;
  auto msg = make_message(std::forward<Ts>(xs)..., sequence_number);
  self->request(dest, std::chrono::milliseconds(200), msg).then(
    [=](ack_atom) { /* nop */ },
    [=](const error&) {
      send_reliably(self, dest, 0, max_retransmits, msg);
    }
  );
}

void send_reliably(stateful_actor<cache>* self, const actor dest,
                   int retransmit_count, int max_retransmits,
                   const caf::message& msg) {
  if (retransmit_count >= max_retransmits) {
    std::cerr << "ERROR: reached max retransmits!" << std::endl;
    return;
  }
  std::cerr << "retransmitting: " << to_string(msg) << std::endl;
  self->request(dest, std::chrono::milliseconds(500), msg).then(
    [=](ack_atom) { /* nop */ },
    [=](const error&) {
      send_reliably(self, dest, retransmit_count + 1, max_retransmits, msg);
    }
  );
}

bool is_duplicate(stateful_actor<cache>* self, uint32_t num) {
  auto& nums = self->state.receiving[self->current_sender()];
  auto res = nums.count(num) > 0;
  nums.insert(num);
  if (res)
    std::cerr << "Ignoring duplicate" << std::endl;
  return res;
}

behavior ping_test(stateful_actor<cache>* self, uint32_t other_nodes,
                   bool leader, const std::string& my_name,
                   int max_retransmits) {
  self->state.received_pongs = 0;
  self->set_default_handler(skip);
  return {
    [=](actor next) {
      std::cout << "[n] " << next.node().process_id() << std::endl;
      self->state.next = next;
      if (leader)
        send_reliably(self, next, max_retransmits, share_atom::value,
                      self, my_name);
      self->set_default_handler(print_and_drop);
      self->become(
        [=](share_atom, actor leader, const std::string& name, uint32_t num) {
          if (!is_duplicate(self, num)) {
            // TODO: Save leader actor and only forward it on received ping
            //       from leader!!!
            auto& s = self->state;
            if (leader == self) {
              std::cout << "[r] actor returned" << std::endl;
            } else {
              std::cout << "[s] " << name << std::endl;
              s.leader = leader;
              //send_reliably(self, s.next, max_retransmits, share_atom::value,
                            //an_actor, name);
              send_reliably(self, leader, max_retransmits, peer_atom::value,
                            self, my_name);
            }
          }
          return ack_atom::value;
        },
        [=](peer_atom, actor peer, std::string& name, uint32_t num) {
          if (!is_duplicate(self, num)) {
            std::cout << "[p] " << name << std::endl;
            send_reliably(self, peer, max_retransmits, ping_atom::value,
                          self, my_name);
          }
          return ack_atom::value;
        },
        [=](ping_atom, actor sender, const std::string& name, uint32_t num) {
          if (!is_duplicate(self, num)) {
            std::cout << "[i] " << name << std::endl;
            send_reliably(self, sender, max_retransmits, pong_atom::value,
                          my_name);
            send_reliably(self, self->state.next, max_retransmits,
                          share_atom::value, self->state.leader, name);
          }
          return ack_atom::value;
        },
        [=](pong_atom, const std::string& name, uint32_t num) {
          if (!is_duplicate(self, num)) {
            std::cout << "[o] " << name << std::endl;
            auto& s = self->state;
            s.received_pongs += 1;
            if (leader && s.received_pongs >= other_nodes)
              send_reliably(self, s.next, max_retransmits, done_atom::value,
                            my_name);
          }
          return ack_atom::value;
        },
        [=](done_atom, const std::string& name, uint32_t num) {
          if (!is_duplicate(self, num)) {
            std::cout << "[d] " << name << std::endl;
            auto&s = self->state;
            s.received_done = true;
            if (leader)
              send_reliably(self, s.next, max_retransmits, shutdown_atom::value,
                            name);
            else
              send_reliably(self, s.next, max_retransmits, done_atom::value,
                            name);
          }
          return ack_atom::value;
        },
        [=](shutdown_atom, const std::string& name, uint32_t num) {
          if (!is_duplicate(self, num)) {
            std::cout << "shutdown!" << std::endl;
            if (!leader)
              send_reliably(self, self->state.next, max_retransmits,
                            shutdown_atom::value, name);
            self->quit();
          }
          return ack_atom::value;
        }
      );
    }
  };
}

} // namespace anonymous

struct protocol_dispatch {
  protocol_dispatch(actor_system& sys, const configuration& config)
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
  std::cout << "Config: \n > host = " << config.host << std::endl
            << " > port = " << config.port << std::endl
            << " > local-port = " << config.local_port << std::endl
            << " > others = " << config.others << std::endl
            << " > offset = " << config.offset << std::endl
            << " > leader = " << std::boolalpha << config.leader << std::endl
            << " > udp = " << std::boolalpha << config.middleman_enable_udp
            << std::endl
            << " > tcp = " << std::boolalpha << config.middleman_enable_tcp
            << std::endl
            << " > timeout = " << config.timeout << std::endl
            << " > retransmit_count = " << config.retransmits << std::endl
            << " > name = " << config.name << std::endl;
  protocol_dispatch pd(system, config);
  auto remote_port = config.port + config.offset;
  auto local_port = config.local_port + config.offset;
  auto name = config.name.empty() ? std::to_string(system.node().process_id())
                                  : config.name;
  if (config.local_port == 0)
    local_port = remote_port;
  std::cout << "Node name = " << name << ", id = " << system.node().process_id()
            << std::endl;
  scoped_actor self{system};
  auto pt = system.spawn(ping_test, config.others, config.leader, name,
                        config.retransmits);
  std::cout << std::endl << "Opening local port ... " << std::endl;
  auto port = pd.publish(pt, local_port, nullptr, true);
  if (!port) {
    std::cerr << "Could not publish my actor on port " << local_port
              << std::endl;
    return;
  }
  std::cout << "Published actor on " << *port << std::endl;

  // Wait for user input. Make sure all participants published their actor.
  if (config.timeout > 0) {
    std::cout << "Waiting for " << config.timeout << " seconds to give all "
                 "nodes a chance to published their actor" << std::endl;
    self->delayed_send(self, std::chrono::seconds(config.timeout),
                       ping_atom::value);
    self->receive([&](ping_atom) {
      std::cout << std::endl << "Connecting to next node ..." << std::endl;
    });
  } else {
    std::cout << "Press any key to continue ... "
                 "(make sure all nodes published their actor)" << std::endl;
    std::cin.get();
    std::cout << std::endl << "Connecting to next node ..." << std::endl;
  }
  auto next = pd.remote_actor(config.host, remote_port);
  if (!next) {
    std::cerr << "Could not connect to next node! (" << config.host << ":"
              << remote_port << ")" << std::endl;
    return;
  }
  std::cout << "Connected." << std::endl << std::endl
            << "Starting interaction ..." << std::endl;
  self->send(pt, *next);
}

CAF_MAIN();
