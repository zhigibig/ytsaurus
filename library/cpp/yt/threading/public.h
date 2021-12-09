#pragma once

namespace NYT::NThreading {

////////////////////////////////////////////////////////////////////////////////

// TODO(babenko): consider increasing to 128 due to cache line pairing in L2 prefetcher.
constexpr size_t CacheLineSize = 64;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NThreading

