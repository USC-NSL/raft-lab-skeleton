#include <cstdint>
#include <ctime>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "common/config.hpp"
#include "toolings/raft_wrapper.hpp"

#include "common/utils/net_intercepter.hpp"

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/spdlog.h"

#include <gtest/gtest.h>

namespace toolings {
using time_point = std::chrono::time_point<std::chrono::system_clock>;

std::once_flag init_flag;

std::string generate_random_string(size_t length) {
  // Character set to choose from (letters and digits)
  const std::string characters =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

  // Random number generator
  std::default_random_engine rng(static_cast<unsigned int>(std::time(0)));
  std::uniform_int_distribution<size_t> dist(0, characters.size() - 1);

  // Generate the random string
  std::string random_string;
  for (size_t i = 0; i < length; ++i) {
    random_string += characters[dist(rng)];
  }

  return random_string;
}

static void DisableLogging(spdlog::logger &logger,
                           bool disable_file_sink = false) {
  // disable console logging
  // file sink won't be affected
  for (auto &sink : logger.sinks()) {
    if (auto console_sink =
            std::dynamic_pointer_cast<spdlog::sinks::stdout_sink_mt>(sink)) {
      console_sink->set_level(spdlog::level::off);
    } else if (auto console_sink = std::dynamic_pointer_cast<
                   spdlog::sinks::stdout_color_sink_mt>(sink)) {
      console_sink->set_level(spdlog::level::off);
    } else if (auto file_sink =
                   std::dynamic_pointer_cast<spdlog::sinks::basic_file_sink_mt>(
                       sink)) {
      if (disable_file_sink)
        file_sink->set_level(spdlog::level::off);
    }
  }
}

[[maybe_unused]] static void DisableLogging(std::set<std::string> ids,
                                            bool disable_file_sink = false) {
  for (const auto &id : ids) {
    auto name = std::format("rafty_node_{}", id);
    auto logger = spdlog::get(name);
    DisableLogging(*logger, disable_file_sink);
  }
}

static void DisableLogging(uint64_t id, bool disable_file_sink = false) {
  auto name = std::format("rafty_node_{}", id);
  auto logger = spdlog::get(name);
  DisableLogging(*logger, disable_file_sink);
}

class TestConfig {
public:
  TestConfig(std::vector<rafty::Config> configs, size_t verbosity = 1)
      : configs(configs), verbosity(verbosity) {
    std::call_once(init_flag, []() {
      if (std::thread::hardware_concurrency() < 2) {
        std::cout << "Warning: only one CPU, which may conceal locking bugs"
                  << std::endl;
      }
    });

    for (const auto &config : this->configs) {
      this->raft_stopped[config.id] = true;
      this->start1(config.id);
    }

    std::set<std::string> ids;
    for (auto &[id, raft] : this->rafts) {
      this->logs[id] = {};
      this->apply_err[id] = "";
      raft->connect_peers();
      this->connected[id] = true;
      ids.insert(std::to_string(id));
    }

    rafty::NetInterceptor::setup_rank(ids);
    rafty::NetInterceptor::set_type(
        rafty::NetInterceptionType::NETWORK_PARTITION);
    rafty::ByteCountingInterceptor::setup_rpc_monitor();

    this->start_time = std::chrono::system_clock::now();
    this->stop.store(false);
    this->failed.store(false);
  }

  ~TestConfig() { this->cleanup(); }

  // begin must be invoked before any other methods
  inline void begin(const std::string &name = "") {
    // logger setup
    auto logger_name = name.empty() ? "raft_test" : "raft_test_" + name;
    this->logger = spdlog::get(logger_name);
    if (!this->logger) {
      // Create the logger if it doesn't exist
      this->logger = spdlog::basic_logger_mt(
          logger_name, std::format("logs/{}.log", logger_name), true);
    }

    this->t0 = std::chrono::system_clock::now();
    this->rpcs0 = 0;
    this->cmds0 = 0;
    this->bytes0 = 0;
    this->max_index = 0;
    this->max_index_0 = 0;

    for (auto &[_, raft] : this->rafts) {
      raft->run();
    }
  }

  inline void crash1(uint64_t i) {
    // TODO: disable connection

    this->mtx.lock();
    if (this->rafts.contains(i)) {
      this->mtx.unlock();
      this->rafts[i]->kill();
      this->ready_queues[i]->close();
      this->mtx.lock();
      this->rafts.erase(i);
      this->ready_queues.erase(i);
      this->connected[i] = false;
      this->raft_stopped[i] = true;
    }
    this->mtx.unlock();
    // this->stop.store(true);
  }

  inline void start1(uint64_t i) {
    this->crash1(i);
    this->mtx.lock();
    this->ready_queues[i] =
        std::make_unique<MessageQueue<rafty::ApplyResult>>(10000);
    this->rafts[i] = std::make_unique<toolings::RaftWrapper>(
        this->configs[i], *this->ready_queues[i]);

    if (verbosity < 2) {
      DisableLogging(i, false);
    }
    if (verbosity < 1) {
      DisableLogging(i, true);
    }
    // start the server
    this->rafts[i]->start_server();
    this->raft_stopped[i] = false;
    this->mtx.unlock();

    std::thread([this, i] {
      this->applier(i, *this->ready_queues[i]);
    }).detach();
  }

  inline void check_timeout() {
    if (!this->failed.load() &&
        std::chrono::system_clock::now() - this->start_time >
            std::chrono::seconds(120)) {
      this->failed.store(true);
      this->logger->critical("test took longer than 120 seconds");
      this->stop.store(true);
      FAIL() << "taking longer than 120 seconds to finish";
    }
  }

  inline void cleanup() {
    this->stop.store(true);
    for (auto &[id, raft] : this->rafts) {
      raft->kill();
      this->ready_queues[id]->close();
    }
  }

  inline std::tuple<std::string, bool> check_logs(uint64_t i,
                                                  const rafty::ApplyResult &m) {
    std::string err_msg = "";
    auto v = m.data;
    for (auto &[id, log] : this->logs) {
      if (log.contains(m.index) && log[m.index] != v) {
        // this->logger->info(
        //     "{}: log {}; server {}", i, m.index, v, log[m.index]
        // );
        err_msg = std::format("commit index={} server={} {} != server={} {}",
                              m.index, i, m.data, id, log[m.index]);
      }
    }
    auto prevok =
        this->logs.contains(i) ? this->logs[i].contains(m.index - 1) : false;
    this->logs[i][m.index] = v;
    if (m.index > this->max_index) {
      this->max_index = m.index;
    }
    return {err_msg, prevok};
  }

  void applier(uint64_t i, MessageQueue<rafty::ApplyResult> &ready_queue) {
    while (true) {
      if (this->stop.load() || this->raft_stopped[i])
        break;
      auto apply_result = ready_queue.dequeue();
      if (this->stop.load() || this->raft_stopped[i])
        break;
      if (!apply_result.valid) {
        // ignore for now...
      } else {
        this->mtx.lock();
        auto [err_msg, preok] = this->check_logs(i, apply_result);
        this->mtx.unlock();
        if (apply_result.index > 1 && !preok) {
          err_msg = std::format("server {} apply out of order {}", i,
                                apply_result.index);
        }
        if (!err_msg.empty()) {
          this->logger->critical("apply error: {}\n", err_msg);
          this->apply_err[i] = err_msg;
          ASSERT_TRUE(false) << "apply error: " << err_msg;
        }
      }
    }
  }

  inline std::optional<uint64_t> check_one_leader() {
    for (auto iters = 0; iters < 10; iters++) {
      {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint64_t> dist(450, 550);
        uint64_t random_number = dist(gen);
        std::this_thread::sleep_for(std::chrono::milliseconds(random_number));
      }

      std::unordered_map<uint64_t, std::vector<uint64_t>> leaders;
      for (auto &[id, raft] : this->rafts) {
        if (!connected[id])
          continue;
        auto state = raft->get_state();
        if (state.is_leader) {
          if (!leaders.contains(state.term)) {
            leaders[state.term] = {id};
          } else {
            leaders[state.term].emplace_back(id);
          }
        }
      }

      auto last_term_with_leader = -1;
      for (auto &[term, ids] : leaders) {
        if (ids.size() > 1) {
          this->logger->critical("term {} has {} (>1) leaders", term,
                                 ids.size());
          EXPECT_FALSE(ids.size() > 1)
              << "term " << term << " has " << ids.size() << " (>1) leaders";
          return std::nullopt;
        }
        if (static_cast<int>(term) > last_term_with_leader) {
          last_term_with_leader = static_cast<int>(term);
        }
      }

      if (!leaders.empty()) {
        return leaders[last_term_with_leader].front();
      }
    }
    this->logger->critical("expected one leader, got none");
    EXPECT_TRUE(false) << "expected one leader, got none";
    return std::nullopt;
  }

  inline std::optional<uint64_t> check_terms() {
    std::optional<uint64_t> term = std::nullopt;
    for (auto &[id, raft] : this->rafts) {
      if (!connected[id])
        continue;
      auto state = raft->get_state();
      auto xterm = state.term;
      if (term == std::nullopt) {
        term = xterm;
      } else if (term != xterm) {
        this->logger->critical("servers disagree on term");
        EXPECT_FALSE(term != xterm) << "servers disagree on term";
        return std::nullopt;
      }
    }
    return term;
  }

  inline void check_no_leader() {
    for (auto &[id, raft] : this->rafts) {
      if (!connected[id])
        continue;
      auto state = raft->get_state();
      if (state.is_leader) {
        this->logger->critical("expected no leader, but {} claims to be leader",
                               id);
        ASSERT_TRUE(false) << "expected no leader, but " << id
                           << " claims to be leader";
      }
    }
  }

  struct CommittedCheck {
    uint64_t num;
    std::string data;
  };

  inline CommittedCheck n_committed(uint64_t i) {
    uint64_t count = 0;
    std::string data = "";
    for (auto &[id, raft] : this->rafts) {
      if (this->apply_err.contains(id) && !this->apply_err[id].empty()) {
        this->logger->critical("server {} apply error: {}", id,
                               this->apply_err[id]);
        // TODO: fail the test
        EXPECT_TRUE(false) << "server " << id
                           << " apply error: " << this->apply_err[id];
        return {0, ""};
      }
      std::lock_guard<std::mutex> lock(this->mtx);
      auto ok = this->logs[id].contains(i);
      if (ok) {
        auto data1 = this->logs[id][i];
        if (count > 0 && data != data1) {
          this->logger->critical(
              "committed values do not match: index {}, {}, {}", i, data,
              data1);
          EXPECT_TRUE(false) << "committed values do not match: index " << i
                             << ", " << data << ", " << data1;
          return {0, ""};
        }
        count++;
        data = data1;
      }
    }
    return {count, data};
  }

  inline std::optional<std::string>
  wait(uint64_t index, uint64_t n,
       std::optional<uint64_t> start_term = std::nullopt) {
    auto to = std::chrono::milliseconds(10);
    for (uint64_t iters = 0; iters < 30; iters++) {
      auto committed = n_committed(index);
      if (committed.num >= n) {
        break;
      }
      std::this_thread::sleep_for(to);
      if (to < std::chrono::seconds(1)) {
        to *= 2;
      }
      if (start_term) {
        for (auto &[id, raft] : this->rafts) {
          auto state = raft->get_state();
          if (state.term > *start_term) {
            return std::nullopt;
          }
        }
      }
    }
    auto committed = n_committed(index);
    if (committed.num < n) {
      this->logger->critical("only {} decided for index {}; wanted {}",
                             committed.num, index, n);
      EXPECT_FALSE(committed.num < n)
          << "only " << committed.num << " decided for index " << index
          << "; wanted " << n;
      return std::nullopt;
    }
    return committed.data;
  }

  inline std::optional<uint64_t> one(std::string data,
                                     uint64_t expected_servers, bool retry) {
    auto t0 = std::chrono::system_clock::now();
    while (std::chrono::system_clock::now() - t0 < std::chrono::seconds(25)) {
      std::optional<uint64_t> index = std::nullopt;
      this->mtx.lock();
      for (auto &[id, raft] : this->rafts) {
        if (!this->connected[id])
          continue;
        auto result = raft->propose(data);
        if (result.is_leader) {
          index = result.index;
          break;
        }
      }
      this->mtx.unlock();

      // somebody claimed to be the leader and to have
      // submitted our command; wait a while for agreement
      if (index) {
        auto t1 = std::chrono::system_clock::now();
        while (std::chrono::system_clock::now() - t1 <
               std::chrono::seconds(5)) {
          auto committed = n_committed(*index);
          if (committed.num > 0 && committed.num >= expected_servers) {
            // committed
            if (committed.data == data) {
              // command check passed.
              return index;
            }
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!retry) {
          this->logger->critical("one ({}) failed to reach agreement", data);
          // TODO: fail the test
          EXPECT_TRUE(false)
              << "one (" << data << ") failed to reach agreement";
          return std::nullopt;
        }
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
    this->logger->critical("one ({}) failed to reach agreement", data);
    EXPECT_TRUE(false) << "one (" << data << ") failed to reach agreement";
    return std::nullopt;
  }

  inline void disconnect(uint64_t id) {
    if (this->rafts.contains(id)) {
      this->logger->info("disconnect {}", id);
      // this->rafts[id]->isolate();
      // rafty::NetPartitionInterceptor::disconnect(std::to_string(id));
      rafty::NetInterceptor::disconnect(std::to_string(id));
      this->connected[id] = false;
    }
  }

  inline void reconnect(uint64_t id) {
    if (this->rafts.contains(id)) {
      this->logger->info("reconnect {}", id);
      // this->rafts[id]->unisolate();
      // rafty::NetPartitionInterceptor::reconnect(std::to_string(id));
      rafty::NetInterceptor::reconnect(std::to_string(id));
      this->connected[id] = true;
    }
  }

  // helper functions for randomly picking n servers
  // returns ids of n servers
  // returned vector size is less than n if there are less than n servers
  inline std::vector<uint64_t> pick_n_servers(uint64_t n,
                                              std::set<uint64_t> exclude) {
    std::vector<uint64_t> picked;
    std::vector<uint64_t> ids;
    for (const auto &[id, _] : this->rafts) {
      if (exclude.find(id) != exclude.end())
        continue;
      ids.push_back(id);
    }
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(ids.begin(), ids.end(), gen);
    for (uint64_t i = 0; i < n && i < ids.size(); i++) {
      picked.push_back(ids[i]);
    }
    return picked;
  }

  inline std::vector<uint64_t>
  pick_n_servers(uint64_t n, std::optional<uint64_t> exclude = std::nullopt) {
    std::set<uint64_t> exclude_set;
    if (exclude) {
      exclude_set.insert(*exclude);
    }
    return pick_n_servers(n, exclude_set);
  }

  inline rafty::ProposalResult propose(uint64_t id, const std::string &data) {
    if (this->rafts.contains(id)) {
      return this->rafts[id]->propose(data);
    }
    throw std::runtime_error("server not found");
  }

public:
  std::shared_ptr<spdlog::logger> logger;
  std::unordered_map<uint64_t, std::unique_ptr<toolings::RaftWrapper>> rafts;

private:
  std::mutex mtx;

  std::vector<rafty::Config> configs;
  std::unordered_map<uint64_t,
                     std::unique_ptr<MessageQueue<rafty::ApplyResult>>>
      ready_queues;

  std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::string>> logs;

  std::unordered_map<uint64_t, std::string> apply_err;

  std::unordered_map<uint64_t, bool> raft_stopped;
  std::unordered_map<uint64_t, bool> connected;

  time_point start_time;
  time_point t0;
  uint64_t rpcs0;
  uint64_t cmds0;
  uint64_t bytes0;
  uint64_t max_index;
  uint64_t max_index_0;

  std::atomic<bool> stop;
  std::atomic<bool> failed;

  size_t verbosity;
};
} // namespace toolings
