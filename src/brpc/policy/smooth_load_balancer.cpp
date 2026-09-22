#include "brpc/policy/smooth_load_balancer.h"
#include "brpc/server_id.h"
#include "butil/strings/string_number_conversions.h"
#include <cstddef>
#include <cstdint>
#include <new>

namespace brpc {
namespace policy {

bool SmoothLoadBalancer::AddServer(const ServerId &id) {
  return _db_servers.Modify(Add, id);
}

bool SmoothLoadBalancer::RemoveServer(const ServerId &id) {
  return _db_servers.Modify(Remove, id);
}

size_t
SmoothLoadBalancer::AddServersInBatch(const std::vector<ServerId> &servers) {
  const size_t n = _db_servers.Modify(BatchAdd, servers);
  LOG_IF(ERROR, n != servers.size()) << "Fail to AddServersInBatch, expected "
                                     << servers.size() << " actually " << n;
  return n;
}

size_t
SmoothLoadBalancer::RemoveServersInBatch(const std::vector<ServerId> &servers) {
  // TODO:
  return 0;
}

int SmoothLoadBalancer::SelectServer(const SelectIn &in, SelectOut *out) {
  // TODO:
  return 0;
}

SmoothLoadBalancer *
SmoothLoadBalancer::New(const butil::StringPiece &params) const {
  // TODO:
  SmoothLoadBalancer *lb = new (std::nothrow) SmoothLoadBalancer;
  return lb;
}

void SmoothLoadBalancer::Destroy() {
  // TODO:
  delete this;
}

void SmoothLoadBalancer::Describe(std::ostream &os,
                                  const DescribeOptions &options) {
  os << "Smooth{";
  os << '}';
}

bool SmoothLoadBalancer::Add(Servers &bg, const ServerId &id) {
  uint32_t initial_weight = 0;

  if (bg.server_list.empty()) {
    initial_weight = MAX_WEIGHT;
  }

  bool insert_server =
      bg.server_map.emplace(id.id, bg.server_list.size()).second;
  if (insert_server) {
    bg.server_list.emplace_back(id.id, initial_weight);
    bg.weight_sum += initial_weight;
    return true;
  }
  return false;
}

bool SmoothLoadBalancer::Remove(Servers &bg, const ServerId &id) {
  auto iter = bg.server_map.find(id.id);
  if (iter != bg.server_map.end()) {
    const size_t idx = iter->second;
    bg.weight_sum -= bg.server_list[idx].weight;
    bg.server_list[idx] = bg.server_list.back();
    bg.server_map[bg.server_list[idx].id] = idx;
    bg.server_list.pop_back();
    bg.server_map.erase(iter);
    return true;
  }
  return false;
}

size_t SmoothLoadBalancer::BatchAdd(Servers &bg,
                                    const std::vector<ServerId> &servers) {

  uint32_t initial_weight = 0;

  if (bg.server_list.empty()) {
    initial_weight = MAX_WEIGHT;
  }

  size_t count = 0;
  for (size_t i = 0; i < servers.size(); ++i) {
    auto &id = servers[i];
    bool insert_server =
        bg.server_map.emplace(id.id, bg.server_list.size()).second;
    if (insert_server) {
      bg.server_list.emplace_back(id.id, initial_weight);
      bg.weight_sum += initial_weight;
      count++;
    }
  }

  return count;
}

size_t SmoothLoadBalancer::BatchRemove(Servers &bg,
                                       const std::vector<ServerId> &servers) {
  size_t count = 0;
  for (size_t i = 0; i < servers.size(); ++i) {
    count += !!Remove(bg, servers[i]);
  }
  return count;
}

} // namespace policy
} // namespace brpc