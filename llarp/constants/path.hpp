#ifndef LLARP_CONSTANTS_PATH_HPP
#define LLARP_CONSTANTS_PATH_HPP

#include <chrono>
#include <cstddef>

#include <util/types.hpp>
#include <util/time.hpp>

namespace llarp
{
  namespace path
  {
    /// maximum path length
    constexpr std::size_t max_len = 8;
    /// default path length
    constexpr std::size_t default_len = 4;
    /// pad messages to the nearest this many bytes
    constexpr std::size_t pad_size = 128;
    /// default path lifetime in ms
    constexpr std::chrono::milliseconds default_lifetime = 20min;
    /// minimum into lifetime we will advertise
    constexpr std::chrono::milliseconds min_intro_lifetime = default_lifetime / 2;
    /// spacing frequency at which we try to build paths for introductions
    constexpr std::chrono::milliseconds intro_path_spread = default_lifetime / 5;
    /// Minimum paths to keep around for intros; mainly used at startup (the
    /// spread, above, should be able to maintain more than this number of paths
    /// normally once things are going).
    constexpr std::size_t min_intro_paths = 4;
    /// after this many ms a path build times out
    constexpr auto build_timeout = 30s;

    /// measure latency every this interval ms
    constexpr auto latency_interval = 20s;
    /// if a path is inactive for this amount of time it's dead
    constexpr auto alive_timeout = latency_interval * 1.5;

    /// how big transit hop traffic queues are
    constexpr std::size_t transit_hop_queue_size = 256;

  }  // namespace path
}  // namespace llarp

#endif
