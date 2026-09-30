#pragma once

#include "brpc/load_balancer.h"
#include "brpc/server_id.h"
#include "brpc/socket_id.h"
#include "butil/containers/doubly_buffered_data.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace brpc {
namespace policy {

class SmoothLoadBalancer : public LoadBalancer {
public:
  bool AddServer(const ServerId &id) override;
  bool RemoveServer(const ServerId &id) override;
  size_t AddServersInBatch(const std::vector<ServerId> &servers) override;
  size_t RemoveServersInBatch(const std::vector<ServerId> &servers) override;
  int SelectServer(const SelectIn &in, SelectOut *out) override;
  SmoothLoadBalancer *New(const butil::StringPiece &) const override;
  void Destroy() override;
  void Describe(std::ostream &, const DescribeOptions &options) override;

private:
  struct Server {
    Server(SocketId id, uint32_t fixed_weight)
        : id(id), initial_weight(fixed_weight), target_weight(fixed_weight),
          generation(0) {}

    Server(SocketId id, uint32_t initial, uint32_t target,
           uint64_t server_generation = 0)
        : id(id), initial_weight(initial), target_weight(target),
          generation(server_generation) {}

    SocketId id;
    uint32_t initial_weight;
    uint32_t target_weight;
    uint64_t
        generation; // 判断相同 SocketId 是否是新一轮加入，防止复用旧预热进度
  };

  struct Servers {
    std::vector<Server> server_list;
    std::map<SocketId, size_t> server_map;
    uint64_t membership_version =
        0; // 只在成员发生变化后扫描并删除 TLS 中已经不属于集群的状态
    uint64_t next_generation = 1;
  };

  struct SelectState {
    uint32_t effective_weight = 0; // 本轮实际权重
    int64_t current_weight = 0;    // 调度过程中累计的权重
    uint64_t generation = 0;
  };

  struct TLS {
    std::map<SocketId, SelectState> states;
    uint64_t observed_membership_version = 0;
  };

  // weight_sum: uint64 -> 2^64/10000
  // = 18TB, uint32 -> 2^32/10000 = 4M
  static const uint32_t MAX_WEIGHT = 10000;

  static bool Add(Servers &bg, const ServerId &id);
  static bool Remove(Servers &bg, const ServerId &id);
  static size_t BatchAdd(Servers &bg, const std::vector<ServerId> &servers);
  static size_t BatchRemove(Servers &bg, const std::vector<ServerId> &servers);

  static SocketId SelectByWeight(const std::vector<Server> &server_list,
                                 TLS &tls);

  butil::DoublyBufferedData<Servers, TLS> _db_servers;
  using TLSScopedPtr = butil::DoublyBufferedData<Servers, TLS>::ScopedPtr;
};

} // namespace policy
} // namespace brpc