#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "common/common.hpp"
#include "common/config.hpp"
#include "common/logger.hpp"

#include "common/utils/net_intercepter.hpp"

#include "toolings/msg_queue.hpp"

#include "raft.grpc.pb.h" // it will pick up correct header
                          // when you generate the grpc proto files

using namespace toolings;

namespace rafty {
using RaftServiceStub = std::unique_ptr<raftpb::RaftService::Stub>;
using grpc::Server;

class Raft {
public:
  // WARN: do not modify the signature of constructor and destructor
  Raft(const Config &config, MessageQueue<ApplyResult> &ready);
  ~Raft();

  // WARN: do not modify the signature
  // TODO: implement `run`, `propose` and `get_state`
  void run();                                      // lab 1
  State get_state() const;                         // lab 1
  ProposalResult propose(const std::string &data); // lab 2

  // WARN: do not modify the signature of
  // `start_server`, `stop_server`, `connect_peers`,
  // `is_dead`, and `kill`.
  void start_server();
  void stop_server();
  void connect_peers();
  bool is_dead() const;
  void kill();

private:
  // WARN: do not modify `create_context` and `apply`.

  // invoke `create_context` when creating context for rpc call.
  // args: the id of which raft instance the RPC will go to.
  std::unique_ptr<grpc::ClientContext> create_context(uint64_t to) const;

  // invoke `apply` when the command/proposal is ready to apply.
  void apply(const ApplyResult &result);

protected:
  // WARN: do not modify `mtx`.
  mutable std::mutex mtx;

private:
  // WARN: do not modify the declaration of
  // `id`, `listening_addr`, `peer_addrs`,
  // `dead`, `ready_queue`, `peers_`, and `server_`.
  uint64_t id;
  std::string listening_addr;
  std::map<uint64_t, std::string> peer_addrs;

  std::atomic<bool> dead;
  MessageQueue<ApplyResult> &ready_queue;

  std::unordered_map<uint64_t, RaftServiceStub> peers_;
  std::unique_ptr<Server> server_;

  // logger is available for you to logging information.
  std::unique_ptr<rafty::utils::logger> logger;
};
} // namespace rafty

#include "rafty/impl/raft.ipp"
