#include <grpc/grpc.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <cstdlib>
#include <ctime>
#include <iostream>

#include "ray/common/asio/periodical_runner.h"
#include "ray/common/component_syncer.h"
using namespace std;
using namespace ray::syncing;

class LocalNode : public Reporter {
 public:
  LocalNode(const std::string &node_id, instrumented_io_context &io_context)
      : io_context_(io_context), node_id_(node_id), timer_(io_context) {
    timer_.RunFnPeriodically(
        [this]() {
          auto v = static_cast<double>(std::rand()) / RAND_MAX;
          if (v < 0.3) {
            int old_state = state_;
            state_ += std::rand() % 10;
            ++version_;
            RAY_LOG(INFO) << node_id_ << " change from (" << old_state
                          << ", v:" << (version_ - 1) << ") to (" << state_
                          << ", v:" << version_ << ")";
          }
        },
        1000);
  }

  ray::rpc::syncer::RaySyncMessage Snapshot() const override {
    ray::rpc::syncer::RaySyncMessage msg;
    msg.set_message_type(ray::rpc::syncer::RaySyncMessageType::SNAPSHOT);
    msg.set_component_id(ray::rpc::syncer::RayComponentId::RESOURCE_MANAGER);
    msg.set_version(version_);
    msg.set_sync_message(
        std::string(reinterpret_cast<const char *>(&state_), sizeof(state_)));
    msg.set_node_id(node_id_);
    RAY_LOG(INFO) << node_id_ << " generate snapshot "
                  << "(" << state_ << ", v:" << version_ << ")";
    return msg;
  }

 private:
  int version_ = 0;
  int state_ = 0;
  instrumented_io_context &io_context_;
  const std::string node_id_;
  ray::PeriodicalRunner timer_;
};

class RemoteNodes : public Receiver {
 public:
  RemoteNodes() {}
  void Update(ray::rpc::syncer::RaySyncMessage &msg) override {
    auto iter = infos_.find(msg.node_id());
    int version = msg.version();
    int state = *reinterpret_cast<const int *>(msg.sync_message().data());
    if (infos_.end() == iter) {
      RAY_LOG(INFO) << "Got a new node " << msg.node_id() << " with (" << state
                    << ", v:" << version << ")";
      infos_[msg.node_id()] = std::make_pair(state, version);
    }
  }

 private:
  absl::flat_hash_map<std::string, std::pair<int, int>> infos_;
};

int main(int argc, char *argv[]) {
  std::srand(std::time(nullptr));
  instrumented_io_context io_context;
  RAY_CHECK(argc == 4) << "./test_syncer_service node_id server_port leader_port";
  auto node_id = std::string(argv[1]);
  auto server_port = std::string(argv[2]);
  auto leader_port = std::string(argv[3]);
  auto local_node = std::make_unique<LocalNode>(node_id, io_context);
  auto remote_node = std::make_unique<RemoteNodes>();
  RaySyncer syncer(node_id, io_context);
  // RPC related field
  grpc::ServerBuilder builder;
  std::unique_ptr<RaySyncerService> service;
  std::unique_ptr<grpc::Server> server;
  std::shared_ptr<grpc::Channel> channel;
  syncer.Register(ray::rpc::syncer::RayComponentId::RESOURCE_MANAGER, local_node.get(),
                  remote_node.get());
  if (server_port != ".") {
    RAY_LOG(INFO) << "Start server on port " << server_port;
    auto server_address = "0.0.0.0:" + server_port;
    service = std::make_unique<RaySyncerService>(syncer);
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(service.get());
    server = builder.BuildAndStart();
  }
  if (leader_port != ".") {
    channel = grpc::CreateChannel("localhost:" + leader_port,
                                  grpc::InsecureChannelCredentials());
    syncer.Follow(channel);
  }
  boost::asio::io_context::work work(io_context);
  io_context.run();

  return 0;
}
