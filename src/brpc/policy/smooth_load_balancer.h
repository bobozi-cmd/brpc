#pragma once

#include "brpc/load_balancer.h"
#include "brpc/server_id.h"
#include "butil/containers/doubly_buffered_data.h"
#include <cstddef>
#include <cstdint>
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
    explicit Server(SocketId s_id = 0, uint32_t s_w = 0)
        : id(s_id), weight(s_w) {}
    SocketId id;
    uint32_t weight;
  };

  struct Servers {
    std::vector<Server> server_list;
    std::map<SocketId, size_t> server_map;
  };

  // weight_sum: uint64 -> 2^64/10000
  // = 18TB, uint32 -> 2^32/10000 = 4M
  static const uint32_t MAX_WEIGHT = 10000;

  static bool Add(Servers &bg, const ServerId &id);
  static bool Remove(Servers &bg, const ServerId &id);
  static size_t BatchAdd(Servers &bg, const std::vector<ServerId> &servers);
  static size_t BatchRemove(Servers &bg, const std::vector<ServerId> &servers);

  butil::DoublyBufferedData<Servers> _db_servers;
};

} // namespace policy
} // namespace brpc