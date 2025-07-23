#include <iostream>
#include <memory>

#include "common/utils/rand_gen.hpp"
#include "rafty/raft.hpp"

namespace rafty {
Raft::Raft(const Config &config, MessageQueue<ApplyResult> &ready)
    : id(config.id), 
      listening_addr(config.addr), 
      peer_addrs(config.peer_addrs),
      dead(false), 
      ready_queue(ready), 
      logger(utils::logger::get_logger(id))
      // TODO: add more field if desired
{
  // TODO: finish it
}

Raft::~Raft() { this->stop_server(); }

void Raft::run() {
  // TODO: kick off the raft instance
  // Note: this function should be non-blocking

  // lab 1
}

State Raft::get_state() const {
  // TODO: finish it
  // lab 1
}

ProposalResult Raft::propose(const std::string &data) {
  // TODO: finish it
  // lab 2
}

// TODO: add more functions if desired.

} // namespace rafty
