#pragma once

#include "rafty/raft.hpp"

namespace toolings {
class RaftWrapper : public rafty::Raft {
public:
  RaftWrapper(const rafty::Config &config,
              MessageQueue<rafty::ApplyResult> &ready)
      : rafty::Raft(config, ready) {}
};
} // namespace toolings
