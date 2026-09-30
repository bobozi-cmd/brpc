#include "brpc/policy/smooth_load_balancer.h"
#include "brpc/excluded_servers.h"
#include "brpc/server_id.h"
#include "brpc/socket.h"
#include "brpc/socket_id.h"
#include "butil/containers/doubly_buffered_data.h"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <utility>
#include <vector>

namespace brpc {
namespace policy {

const uint32_t SmoothLoadBalancer::MAX_WEIGHT;

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
  return _db_servers.Modify(BatchRemove, servers);
}

SocketId
SmoothLoadBalancer::SelectByWeight(const std::vector<Server> &server_list,
                                   TLS &tls) {

  if (server_list.empty()) {
    return INVALID_SOCKET_ID;
  }

  size_t maxi = 0;
  int64_t total_w = 0;
  int64_t max_w = std::numeric_limits<int64_t>::min();

  for (size_t i = 0; i < server_list.size(); ++i) {
    const auto &server = server_list[i];
    const auto &id = server.id;
    auto result = tls.states.emplace(id, SelectState());
    SelectState &state = result.first->second;

    if (result.second || state.generation != server.generation) {
      state = SelectState();
      state.effective_weight = server.initial_weight;
      state.generation = server.generation;
    }

    if (tls.states[id].effective_weight < server.target_weight) {
      ++state.effective_weight;
    }

    state.current_weight += state.effective_weight;
    total_w += state.effective_weight;
    if (state.current_weight > max_w) {
      max_w = state.current_weight;
      maxi = i;
    }
  }

  auto max_socket_id = server_list[maxi].id;
  // 被选节点：current_weight -= 所有 effective_weight 之和
  tls.states[max_socket_id].current_weight -= total_w;

  return max_socket_id;
}

int SmoothLoadBalancer::SelectServer(const SelectIn &in, SelectOut *out) {
  TLSScopedPtr s;
  if (_db_servers.Read(&s) != 0) {
    return ENOMEM;
  }
  const size_t n = s->server_list.size();
  TLS &tls = s.tls();

  if (tls.observed_membership_version != s->membership_version) {
    for (auto it = tls.states.begin(); it != tls.states.end();) {
      if (s->server_map.find(it->first) == s->server_map.end()) {
        it = tls.states.erase(it);
      } else {
        ++it;
      }
    }
    tls.observed_membership_version = s->membership_version;
  }

  if (n == 0) {
    return ENODATA;
  }

  std::vector<Server> candidates;
  std::vector<SocketUniquePtr> candidate_sockets;

  for (size_t i = 0; i < n; ++i) {
    const Server &server = s->server_list[i];
    SocketUniquePtr ptr;
    if (!ExcludedServers::IsExcluded(in.excluded, server.id) &&
        IsServerAvailable(server.id, &ptr)) {
      candidates.push_back(server);
      candidate_sockets.push_back(std::move(ptr));
    }
  }

  if (candidates.empty()) {
    for (size_t i = 0; i < n; ++i) {
      const Server &server = s->server_list[i];
      SocketUniquePtr ptr;
      if (IsServerAvailable(server.id, &ptr)) {
        candidates.push_back(server);
        candidate_sockets.push_back(std::move(ptr));
      }
    }
  }

  if (!candidates.empty()) {
    const auto target_id = SelectByWeight(candidates, tls);
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (candidates[i].id == target_id) {
        *out->ptr = std::move(candidate_sockets[i]);
        return 0;
      }
    }
  }

  return EHOSTDOWN;
}

SmoothLoadBalancer *
SmoothLoadBalancer::New(const butil::StringPiece &params) const {
  SmoothLoadBalancer *lb = new (std::nothrow) SmoothLoadBalancer;
  return lb;
}

void SmoothLoadBalancer::Destroy() { delete this; }

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
    bg.server_list.emplace_back(id.id, initial_weight, MAX_WEIGHT,
                                bg.next_generation++);
    ++bg.membership_version;
    return true;
  }
  return false;
}

bool SmoothLoadBalancer::Remove(Servers &bg, const ServerId &id) {
  auto iter = bg.server_map.find(id.id);
  if (iter != bg.server_map.end()) {
    const size_t idx = iter->second;
    bg.server_list[idx] = bg.server_list.back();
    bg.server_map[bg.server_list[idx].id] = idx;
    bg.server_list.pop_back();
    bg.server_map.erase(iter);
    ++bg.membership_version; // BatchRemove 当前复用 Remove，会让版本增加多次, 不影响正确性
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
      bg.server_list.emplace_back(id.id, initial_weight, MAX_WEIGHT, bg.next_generation++);
      count++;
    }
  }

  if (count != 0) { // 批量添加只增加一次成员版本
    ++bg.membership_version;
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