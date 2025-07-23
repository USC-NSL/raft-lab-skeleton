#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_split.h"
#include "common/utils/net_intercepter.hpp"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/spdlog.h"
#include "toolings/config_gen.hpp"

ABSL_FLAG(uint64_t, num, 3, "number of nodes to spawn (>= 3)");
ABSL_FLAG(std::string, bin, "./node", "the binary of node app");
ABSL_FLAG(int, verbosity, 1,
          "Verbosity level: 0 (silent), 1 (raft message (file sink only))");
ABSL_FLAG(int, fail_type, 0, "Failure Type: 0 (disonnection), 1 (partition)");

static void DisableConsoleLogging(spdlog::logger &logger,
                                  bool disable_file_sink = false) {
  // disable console logging
  // file sink won't be affected unless specified
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

void prepare_logging(uint64_t id) {
  int verbosity = absl::GetFlag(FLAGS_verbosity);
  auto logger_name = std::format("rafty_node_{}", static_cast<uint64_t>(id));
  if (verbosity < 2) {
    DisableConsoleLogging(*spdlog::get(logger_name), false);
  }
  if (verbosity < 1) {
    DisableConsoleLogging(*spdlog::get(logger_name), true);
  }
}

void prepare_interceptor(const std::set<std::string> &world) {
  // setup network interceptor
  rafty::NetInterceptor::setup_rank(world);
  int failure = absl::GetFlag(FLAGS_fail_type);
  if (failure == 0) {
    rafty::NetInterceptor::set_type(
        rafty::NetInterceptionType::NETWORK_FAILURE);
  } else if (failure == 1) {
    rafty::NetInterceptor::set_type(
        rafty::NetInterceptionType::NETWORK_PARTITION);
  } else {
    rafty::NetInterceptor::set_type(
        rafty::NetInterceptionType::NETWORK_FAILURE);
  }
}

class Process {
public:
  Process(const std::string &binary_path,
          const std::vector<std::string> &args) {
    if (pipe(pipe_stdin) == -1 || pipe(pipe_stdout) == -1) {
      throw std::runtime_error("Failed to create pipes");
    }

    pid = fork();
    if (pid == -1) {
      throw std::runtime_error("Failed to fork process");
    }

    if (pid == 0) { // Child process
      // Replace stdin and stdout with the pipe ends
      dup2(pipe_stdin[0], STDIN_FILENO);
      dup2(pipe_stdout[1], STDOUT_FILENO);

      // Close unused pipe ends
      close(pipe_stdin[0]);
      close(pipe_stdin[1]);
      close(pipe_stdout[0]);
      close(pipe_stdout[1]);

      // Prepare arguments for exec
      std::vector<char *> exec_args;
      exec_args.push_back(const_cast<char *>(binary_path.c_str()));
      for (const auto &arg : args) {
        exec_args.push_back(const_cast<char *>(arg.c_str()));
      }
      exec_args.push_back(nullptr); // Null-terminate the argument list

      execvp(binary_path.c_str(), exec_args.data());
      // If execvp fails
      perror("error");
      exit(1);
    } else { // Parent process
      // Close unused pipe ends
      close(pipe_stdin[0]);
      close(pipe_stdout[1]);
    }
  }

  ~Process() = default;

  void send_cmd(const std::string &command) {
    auto _ __attribute__((unused)) = write(pipe_stdin[1], command.c_str(), command.size());
    auto __ __attribute__((unused)) = write(pipe_stdin[1], "\n", 1);
    // manually silence unused warning, but can be dangerous for
    // not handling error properly...
  }

  std::string read_resp() {
    char buffer[128];
    ssize_t count = read(pipe_stdout[0], buffer, sizeof(buffer) - 1);
    if (count > 0) {
      buffer[count] = '\0';
      return std::string(buffer);
    }
    return "";
  }

  void cleanup() {
    close(pipe_stdin[1]);
    close(pipe_stdout[0]);
    waitpid(pid, nullptr, 0); // Wait for the child process to finish
  }

private:
  int pipe_stdin[2];
  int pipe_stdout[2];
  pid_t pid;
};

void command_loop(std::vector<Process> &procs) {
  std::string command;
  std::cout << "Enter commands: "
               "\n\tr \tstart multinode raft cluster, "
               "\n\tdis <id1{, id2}> \tdisconnect a node from the cluster, "
               "\n\tconn <id1{, id2}> \treconnect a node into the cluster, "
               "\n\tk \tkill the multinode raft cluster"
            << std::endl;

  while (true) {
    std::cout << "> ";
    std::getline(std::cin, command);

    auto command_ = absl::StripAsciiWhitespace(command);
    std::vector<std::string> splits = absl::StrSplit(command_, " ");
    if (splits.empty())
      continue;
    if (splits.size() == 1) {
      auto cmd = splits.front();
      if (cmd == "r") {
        for (auto &p : procs) {
          p.send_cmd(cmd);
        }
        continue;
      } else if (cmd == "k") {
        for (auto &p : procs) {
          p.send_cmd(cmd);
        }
        break;
      } else if (cmd.empty()) {
        continue;
      } else {
        std::cout << "Unknown command: " << cmd << std::endl;
      }
    } else if (splits.size() >= 2) {
      auto cmd = splits[0];
      if (cmd == "dis" || cmd == "conn") {
        for (auto &p : procs) {
          p.send_cmd(command);
        }
      } else {
        std::cout << "Unknown command: " << command_ << std::endl;
      }
    } else {
      std::cout << "Unknown command: " << command_ << std::endl;
    }
  }

  std::cout << "exiting..." << std::endl;
}

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  auto num = absl::GetFlag(FLAGS_num);
  auto binary_path = absl::GetFlag(FLAGS_bin);
  auto fail_type = absl::GetFlag(FLAGS_fail_type);
  auto verbosity = absl::GetFlag(FLAGS_verbosity);

  auto insts = toolings::ConfigGen::gen_local_instances(num, 50050);

  std::vector<Process> processes;
  for (const auto &inst : insts) {
    std::vector<std::string> args = {
        "--id",
        std::to_string(inst.id),
        "--port",
        std::to_string(inst.port),
        "--fail_type",
        std::to_string(fail_type),
        "--verbosity",
        std::to_string(std::max(verbosity, 1)), // max 1 as verbosity
        "--peers"};
    std::string peer_addr = "";
    for (const auto &peer : insts) {
      if (peer.id == inst.id)
        continue;
      peer_addr += std::format("{}+{},", peer.id, peer.external_addr);
    }
    auto peer_addr_ = absl::StripSuffix(peer_addr, ",");
    args.emplace_back(peer_addr_);
    std::cout << "execute: " << binary_path << " ";
    for (const auto &arg : args) {
      std::cout << arg << " ";
    }
    std::cout << std::endl;
    processes.emplace_back(binary_path, args);
  }
  command_loop(processes);

  for (auto &proc : processes) {
    proc.cleanup();
  }
  return 0;
}