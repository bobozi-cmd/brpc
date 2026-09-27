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
        : id(id), initial_weight(fixed_weight), target_weight(fixed_weight) {}

    Server(SocketId id, uint32_t initial, uint32_t target)
        : id(id), initial_weight(initial), target_weight(target) {}

    SocketId id;
    uint32_t initial_weight;
    uint32_t target_weight;
  };

  struct Servers {
    std::vector<Server> server_list;
    std::map<SocketId, size_t> server_map;
  };

  struct SelectState {
    uint32_t effective_weight = 0; // 本轮实际权重
    int64_t current_weight = 0;    // 调度过程中累计的权重
  };

  struct TLS {
    std::map<SocketId, SelectState> states;
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