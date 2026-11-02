// Pure tunnel-interface policy shared by the Linux daemon and its unit tests.
//
// The tunnel is DUAL-STACK (connect/IPV6.md C2): remote providers carry v4
// and v6, so the platform tunnel advertises one address of each family, a
// capture route set for each family, and resolvers of each family. The
// predicate below is the fail-closed floor Tunnel::Open enforces BEFORE any
// device exists: a configuration missing either half is refused outright
// rather than brought up as a silent split tunnel (v6 leaving in the clear
// while the UI says Connected is exactly the leak the v6 half exists to stop).
//
// Everything here is plain C++17 with no syscalls, so the address checks and
// the capture set are testable on any developer machine (tests/TunnelPolicyTest.cpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace urnw {

// The per-device memory target the daemon passes to
// newDeviceLocalWithMemoryTarget (daemon/TunnelHost.cpp), and the process
// budget it passes to setMemoryLimit (daemon/main.cpp). The two are one
// decision: connect draws the H3 carrier windows from the device target (the
// stream window is three quarters of the carrier's eighth, so 3 * target / 32),
// while the device target itself has to be backed by the process budget.
//
// Two constraints bind the pair, and this one satisfies both (asserted below):
//
//   backing    the device targets plus the message pools must fit the process
//              budget, and the pools take 14 of 34 parts, so a target may be at
//              most 20/34 of the budget. 128 MiB <= 20/34 * 384 MiB = 225.9.
//   collector  setMemoryLimit is also the go soft limit, and live heap
//              amplifies about threefold at the runtime; a target too close to
//              its soft limit reproduces the measured mobile collection storm
//              (23.6 collections per second). The budget is therefore at least
//              three times the target: 384 MiB = 3 * 128 MiB.
//
// urnetworkd.service declares no MemoryMax or MemoryHigh, so nothing below
// these values caps the daemon. The GUI (src/SdkHost.cpp) keeps its own,
// smaller bound: it owns a DeviceRemote and no data plane.
//
// There are two tiers. The base pair is the floor; a host measuring more than
// kLargeHostMemoryByteCount of usable memory gets the large pair, which doubles
// both numbers and so doubles the H3 stream window again. The tier is
// chosen from MEASURED host memory (daemon/HostMemory.hpp reads /proc/meminfo
// and any cgroup limit), never from an assumption, and an unmeasurable host
// takes the base pair: an unknown host is not a large host.
inline constexpr std::int64_t kDeviceMemoryTargetByteCount = 128LL * 1024 * 1024;
inline constexpr std::int64_t kProcessMemoryBudgetByteCount = 384LL * 1024 * 1024;
inline constexpr std::int64_t kLargeHostDeviceMemoryTargetByteCount = 256LL * 1024 * 1024;
inline constexpr std::int64_t kLargeHostProcessMemoryBudgetByteCount = 768LL * 1024 * 1024;
// THE BAR: every machine sold as 8 GiB or more takes the large tier. The same
// bar on macOS, Windows and this daemon.
//
// This is a product decision rather than a memory one: the throughput the
// larger window buys is the product, and it is wanted on ordinary machines
// rather than on workstations alone. It is a deliberate trade, and worth
// stating as one. The 256 MiB target permits a 768 MiB process budget, and that
// budget is the go soft limit -- not a ceiling the process avoids, but the
// level the collector lets live heap climb toward before it works hard. On a
// machine just over this bar, a steady-state daemon approaching that figure is
// a real share of the machine. The program spends that memory because the
// throughput is what it is buying.
//
// Two things a later reader will need if the bar is ever revisited. Host memory
// is a weak proxy for a link fast enough to make the receive window bind, so
// some hosts over the bar -- laptops on wireless, mostly -- pay the memory and
// never reach the throughput it buys. And the instrument that would size this
// on the thing that actually predicts the need is an explicit opt-in, or
// promotion on measured throughput, rather than any RAM threshold; either is a
// different change from this one.
//
// "Usable" matters here: HostMemoryByteCountCached takes the smaller of
// physical memory and the cgroup limit, so a daemon in a 4 GiB container on a
// 64 GiB machine is a small host.
//
// WHY THE NUMBER BELOW IS 7 AND NOT 8 -- do not round it up. A probe reports
// usable memory, and usable memory is below the size a machine is sold as:
// firmware, the kernel and an integrated GPU's carve-out come off before
// /proc/meminfo reports MemTotal, so a machine sold as 8 GiB reads roughly
// 7.6-7.8 GiB. A bar set at the nominal 8 GiB would exclude exactly the
// machines it exists to include, and would silently send every base-model 8 GiB
// machine back to the small tier. The threshold sits a whole GiB below nominal
// so that cannot happen, while anything genuinely smaller -- a 6 GiB machine
// reads about 5.7 -- stays base.
//
// The comparison is strict -- measured memory must EXCEED 7 GiB -- which sits
// naturally with a threshold chosen below the nominal size. Rounding this up is
// not left to review: the static_asserts below require an exact 8 GiB reading
// and a realistic 7.68 GiB one to take the large tier, so the build fails.
//
// The GUI (src/SdkHost.cpp) has no tier: it owns a DeviceRemote and no data
// plane, so it sizes no device and keeps its own small fixed budget. If it ever
// gains a data plane, this bar is its bar.
inline constexpr std::int64_t kLargeHostMemoryByteCount = 7LL * 1024 * 1024 * 1024;

// What machines sold as 8 GiB actually report, for those assertions: installed
// memory exactly, and a realistic usable reading after firmware, kernel and
// integrated-graphics reservations (a MemTotal of 8048972 kB, about 7.68 GiB).
inline constexpr std::int64_t kNominal8GiBHostMemoryByteCount = 8LL * 1024 * 1024 * 1024;
inline constexpr std::int64_t kUsable8GiBHostMemoryByteCount = 8048972LL * 1024;

// The parts the two constraints are written in, so a pair cannot drift apart
// silently.
inline constexpr std::int64_t kMemoryPoolRatioParts = 14;
inline constexpr std::int64_t kMemoryBudgetRatioParts = 34;
inline constexpr std::int64_t kCollectorBudgetMultiple = 3;

// One tier: the device target and the process budget that backs it, which are
// only ever chosen together.
struct MemoryTier {
  std::int64_t device_target_byte_count;
  std::int64_t process_budget_byte_count;
};

// The tier for a host with `hostMemoryByteCount` usable bytes. A nonpositive
// (unknown) measurement takes the base tier, which is now the RARE path --
// genuinely small hosts, and hosts whose probe failed -- and so the one to keep
// pinned by assertions rather than by practice.
constexpr MemoryTier MemoryTierForHost(std::int64_t hostMemoryByteCount) {
  if (kLargeHostMemoryByteCount < hostMemoryByteCount) {
    return MemoryTier{kLargeHostDeviceMemoryTargetByteCount,
                      kLargeHostProcessMemoryBudgetByteCount};
  }
  return MemoryTier{kDeviceMemoryTargetByteCount, kProcessMemoryBudgetByteCount};
}

constexpr bool MemoryTierIsBacked(MemoryTier tier) {
  return tier.device_target_byte_count * kMemoryBudgetRatioParts <=
         tier.process_budget_byte_count * (kMemoryBudgetRatioParts - kMemoryPoolRatioParts);
}

constexpr bool MemoryTierIsCollectorSafe(MemoryTier tier) {
  return kCollectorBudgetMultiple * tier.device_target_byte_count <=
         tier.process_budget_byte_count;
}

// Both constraints, on both tiers, at compile time: raising one number without
// the other fails the build rather than the fleet.
static_assert(MemoryTierIsBacked(MemoryTierForHost(0)),
              "the base device memory target is not backed by its process budget");
static_assert(MemoryTierIsCollectorSafe(MemoryTierForHost(0)),
              "the base process budget is too close to its device memory target");
// Both constraints on the tiers either side of the bar itself, not merely on
// some small and some large host.
static_assert(MemoryTierIsBacked(MemoryTierForHost(kLargeHostMemoryByteCount)),
              "the tier at the bar is not backed by its process budget");
static_assert(MemoryTierIsCollectorSafe(MemoryTierForHost(kLargeHostMemoryByteCount)),
              "the tier at the bar has a process budget too close to its target");
static_assert(MemoryTierIsBacked(MemoryTierForHost(kLargeHostMemoryByteCount + 1)),
              "the large-host device memory target is not backed by its process budget");
static_assert(MemoryTierIsCollectorSafe(MemoryTierForHost(kLargeHostMemoryByteCount + 1)),
              "the large-host process budget is too close to its device memory target");
// The base tier is the rare path now, so its WHOLE pair is asserted rather than
// left to practice: an unknown host gets the base target and the base budget.
static_assert(MemoryTierForHost(0).device_target_byte_count == kDeviceMemoryTargetByteCount,
              "an unknown host must take the base device memory target");
static_assert(MemoryTierForHost(0).process_budget_byte_count == kProcessMemoryBudgetByteCount,
              "an unknown host must take the base process budget");
static_assert(MemoryTierForHost(-1).device_target_byte_count == kDeviceMemoryTargetByteCount &&
                  MemoryTierForHost(-1).process_budget_byte_count == kProcessMemoryBudgetByteCount,
              "an unmeasurable host must take the base memory tier");
// The comparison is strict, and the three rows around the bar pin it: one byte
// under and exactly at the bar are base, one byte over is large.
static_assert(MemoryTierForHost(kLargeHostMemoryByteCount - 1).device_target_byte_count ==
                  kDeviceMemoryTargetByteCount,
              "a host one byte under the bar must take the base memory tier");
static_assert(MemoryTierForHost(kLargeHostMemoryByteCount).device_target_byte_count ==
                  kDeviceMemoryTargetByteCount,
              "a host exactly at the bar must take the base memory tier");
static_assert(MemoryTierForHost(kLargeHostMemoryByteCount + 1).device_target_byte_count ==
                  kLargeHostDeviceMemoryTargetByteCount,
              "a host one byte over the bar must take the large memory tier");
// The machines the decision is about. These are what fail if the bar is ever
// rounded up to the nominal 8 GiB.
static_assert(MemoryTierForHost(kNominal8GiBHostMemoryByteCount).device_target_byte_count ==
                      kLargeHostDeviceMemoryTargetByteCount &&
                  MemoryTierForHost(kNominal8GiBHostMemoryByteCount).process_budget_byte_count ==
                      kLargeHostProcessMemoryBudgetByteCount,
              "a machine reporting exactly 8 GiB must take the large memory tier");
static_assert(MemoryTierForHost(kUsable8GiBHostMemoryByteCount).device_target_byte_count ==
                      kLargeHostDeviceMemoryTargetByteCount &&
                  MemoryTierForHost(kUsable8GiBHostMemoryByteCount).process_budget_byte_count ==
                      kLargeHostProcessMemoryBudgetByteCount,
              "a machine sold as 8 GiB, reporting its usable 7.68 GiB, must take the large tier");

// ---- host memory measurement (pure halves) ---------------------------------
//
// The daemon's probe does the file reads (daemon/HostMemory.cpp); everything
// that decides anything is here, where the unit tests can reach it. Same shape
// as the provider's own host-memory helper (sn/miner/provider_memory.go).

// MemTotal (KiB) from /proc/meminfo content, in bytes. 0 when absent or
// unparseable.
inline std::int64_t ParseMeminfoTotalByteCount(std::string_view meminfo) {
  constexpr std::string_view kKey = "MemTotal:";
  for (std::size_t at = 0; at < meminfo.size();) {
    const std::size_t end = meminfo.find('\n', at);
    const std::string_view line =
        meminfo.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
    at = (end == std::string_view::npos) ? meminfo.size() : end + 1;
    if (line.substr(0, kKey.size()) != kKey) continue;

    std::int64_t kib = 0;
    bool anyDigit = false;
    for (const char c : line.substr(kKey.size())) {
      if (c == ' ' || c == '\t') {
        if (anyDigit) break;
        continue;
      }
      if (c < '0' || '9' < c) break;
      anyDigit = true;
      // A malformed line with absurd digits must not wrap into a large host.
      if (kib > (std::int64_t{1} << 52)) return 0;
      kib = kib * 10 + (c - '0');
    }
    return anyDigit && 0 < kib ? kib * 1024 : 0;
  }
  return 0;
}

// A cgroup v2 memory.max or v1 memory.limit_in_bytes. "max" and the v1
// unlimited sentinel (a page-rounded int64 max) mean no limit, which is 0.
inline std::int64_t ParseCgroupMemoryLimitByteCount(std::string_view content) {
  std::size_t begin = 0;
  std::size_t end = content.size();
  while (begin < end && (content[begin] == ' ' || content[begin] == '\n' || content[begin] == '\t'))
    ++begin;
  while (begin < end &&
         (content[end - 1] == ' ' || content[end - 1] == '\n' || content[end - 1] == '\t'))
    --end;
  const std::string_view value = content.substr(begin, end - begin);
  if (value.empty() || value == "max") return 0;

  std::int64_t byteCount = 0;
  for (const char c : value) {
    if (c < '0' || '9' < c) return 0;
    if (byteCount > (std::int64_t{1} << 62)) return 0;
    byteCount = byteCount * 10 + (c - '0');
  }
  if (byteCount <= 0 || (std::int64_t{1} << 62) <= byteCount) return 0;
  return byteCount;
}

// What this process can actually use: the smaller of physical memory and any
// cgroup limit, so a daemon in a memory-limited container sizes itself to the
// container rather than to the machine.
constexpr std::int64_t EffectiveHostMemoryByteCount(std::int64_t physicalByteCount,
                                                   std::int64_t limitByteCount) {
  if (physicalByteCount <= 0) return limitByteCount > 0 ? limitByteCount : 0;
  if (0 < limitByteCount && limitByteCount < physicalByteCount) return limitByteCount;
  return physicalByteCount;
}

// Mirrors sdk.GetDefaultTunnelMtu / connect.DefaultTunnelMtu: the INTERFACE
// MTU. It is the IPv6 minimum link MTU, because Linux disables IPv6 on an
// interface whose MTU is below 1280, and the v6 half of the tunnel needs the
// link to exist. The packet-size contract is separate and smaller
// (connect.DefaultMtu = 1100, the largest packet a provider builds), so one
// full encrypted tunnel packet plus the UR envelope still fits one initial H3
// QUIC DATAGRAM.
inline constexpr int kIpv6MinimumMtu = 1280;
inline constexpr int kTunnelMtu = kIpv6MinimumMtu;

struct TunnelConfig {
  std::string name = "urnet0";
  std::string local_addr_v4 = "169.254.2.1";
  int prefix_v4 = 24;
  // The v6 half. The device draws the address from the SDK's fixed ULA /48
  // (sdk tunnel_address_ipv6.go); TunnelHost substitutes a fixed ULA when the
  // device reports an unusable one, exactly as it does for the v4 address.
  std::string local_addr_v6 = "fd00:7572:6e65:ffff::2";
  int prefix_v6 = 64;
  int mtu = kTunnelMtu;
  std::vector<std::string> dns_servers_v4;
  std::vector<std::string> dns_servers_v6;
  // FORK ADDITION (not upstream). Refuse to install the capture routes unless
  // the daemon's own sockets are demonstrably steered around them. Only a dev
  // run (URNETWORK_ALLOW_UNPROTECTED_EGRESS=1) may clear it, and clearing it is
  // logged loudly, because the alternative is a tunnel that comes up and
  // carries nothing while the UI says Connected. IsDualStackTunnelConfig
  // deliberately ignores this field.
  bool require_egress_protection = true;
};

// Whether nftables may use its socket-cgroup expression for the daemon's
// egress exemption. Some otherwise-capable kernels (notably Docker Desktop's
// LinuxKit kernel) ship cgroup BPF but omit CONFIG_NFT_SOCKET. In that case a
// proven socket-creation mark is sufficient for a floorless tunnel, but it is
// not sufficient for a crash-safe kill-switch floor or for a DNS helper that
// lives in another cgroup.
enum class NftCgroupMode {
  CgroupAndMark,
  MarkOnly,
  Refuse,
};

// Pure policy boundary for the runtime kernel probe. Refusal is intentional:
// omitting an unavailable expression must never silently weaken the states
// whose safety depends on matching another cgroup or surviving daemon death.
constexpr NftCgroupMode SelectNftCgroupMode(bool socketMarkerProven,
                                            bool cgroupSocketMatchSupported,
                                            bool blockFloor,
                                            bool helperDnsRequired) {
  if (cgroupSocketMatchSupported) return NftCgroupMode::CgroupAndMark;
  if (!socketMarkerProven || blockFloor || helperDnsRequired) {
    return NftCgroupMode::Refuse;
  }
  return NftCgroupMode::MarkOnly;
}

constexpr bool IsIpv4Literal(std::string_view address) {
  if (address.empty()) return false;

  int octets = 0;
  int digits = 0;
  int value = 0;
  for (const char c : address) {
    if (c == '.') {
      if (digits == 0 || value > 255 || octets == 3) return false;
      ++octets;
      digits = 0;
      value = 0;
      continue;
    }
    if (c < '0' || c > '9' || digits == 3) return false;
    value = value * 10 + (c - '0');
    ++digits;
  }
  return octets == 3 && digits != 0 && value <= 255;
}

// ---- IPv6 literals ---------------------------------------------------------

// A parsed IPv6 address: the 16 network-order bytes.
struct Ipv6Bytes {
  unsigned char b[16] = {};
};

namespace tunnelpolicy_detail {

constexpr int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

// One hextet ("0" .. "ffff") into its value; -1 when it is not one.
constexpr int ParseHextet(std::string_view group) {
  if (group.empty() || group.size() > 4) return -1;
  int value = 0;
  for (const char c : group) {
    const int digit = HexValue(c);
    if (digit < 0) return -1;
    value = value * 16 + digit;
  }
  return value;
}

// A dotted-quad tail ("1.2.3.4") into two hextets. false when it is not one.
constexpr bool ParseEmbeddedIpv4(std::string_view tail, int* high, int* low) {
  if (!IsIpv4Literal(tail)) return false;
  int octets[4] = {0, 0, 0, 0};
  int index = 0;
  int value = 0;
  for (const char c : tail) {
    if (c == '.') {
      octets[index++] = value;
      value = 0;
      continue;
    }
    value = value * 10 + (c - '0');
  }
  octets[index] = value;
  *high = octets[0] * 256 + octets[1];
  *low = octets[2] * 256 + octets[3];
  return true;
}

}  // namespace tunnelpolicy_detail

// Parses a textual IPv6 address (RFC 4291 §2.2: hextets, one "::", an
// optional embedded dotted-quad tail). No brackets, no zone id, no prefix
// length: a value carrying any of those is not an address and is refused.
// Returns false without touching *out when the text is not an address.
constexpr bool ParseIpv6Literal(std::string_view address, Ipv6Bytes* out) {
  using namespace tunnelpolicy_detail;
  if (address.empty() || address.size() > 45) return false;

  int head[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // hextets before "::"
  int tail[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // hextets after "::"
  int headCount = 0;
  int tailCount = 0;
  bool compressed = false;

  size_t pos = 0;
  // a leading "::" (a leading ':' that is not "::" is malformed)
  if (address[0] == ':') {
    if (address.size() < 2 || address[1] != ':') return false;
    compressed = true;
    pos = 2;
    if (pos == address.size()) {
      // "::" alone: all zeros
      *out = Ipv6Bytes{};
      return true;
    }
  }

  while (pos < address.size()) {
    // the next group runs to the next ':' or the end
    size_t end = address.find(':', pos);
    if (end == std::string_view::npos) end = address.size();
    const std::string_view group = address.substr(pos, end - pos);
    if (group.empty()) return false;  // ":::" or a trailing single ':'

    int* target = compressed ? tail : head;
    int* count = compressed ? &tailCount : &headCount;

    if (group.find('.') != std::string_view::npos) {
      // an embedded dotted quad is only legal as the LAST group
      if (end != address.size()) return false;
      int high = 0;
      int low = 0;
      if (!ParseEmbeddedIpv4(group, &high, &low)) return false;
      if (headCount + tailCount + 2 > 8) return false;
      target[(*count)++] = high;
      target[(*count)++] = low;
      pos = end;
      break;
    }

    const int value = ParseHextet(group);
    if (value < 0) return false;
    if (headCount + tailCount + 1 > 8) return false;
    target[(*count)++] = value;

    if (end == address.size()) {
      pos = end;
      break;
    }
    // at a ':': either a single separator or the "::"
    pos = end + 1;
    if (pos < address.size() && address[pos] == ':') {
      if (compressed) return false;  // a second "::"
      compressed = true;
      ++pos;
      if (pos == address.size()) break;  // a trailing "::"
    } else if (pos == address.size()) {
      return false;  // a trailing single ':'
    }
  }

  const int total = headCount + tailCount;
  if (compressed) {
    // "::" stands for at least one zero group
    if (total > 7) return false;
  } else if (total != 8) {
    return false;
  }

  int groups[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < headCount; ++i) groups[i] = head[i];
  for (int i = 0; i < tailCount; ++i) groups[8 - tailCount + i] = tail[i];
  for (int i = 0; i < 8; ++i) {
    out->b[2 * i] = static_cast<unsigned char>(groups[i] >> 8);
    out->b[2 * i + 1] = static_cast<unsigned char>(groups[i] & 0xff);
  }
  return true;
}

constexpr bool IsIpv6Literal(std::string_view address) {
  Ipv6Bytes ignored;
  return ParseIpv6Literal(address, &ignored);
}

// Whether `address` lies inside `prefix` ("2001:db8::/32"). false when either
// is malformed, or when the prefix length is outside 0..128.
constexpr bool Ipv6PrefixContains(std::string_view prefix, std::string_view address) {
  const size_t slash = prefix.find('/');
  if (slash == std::string_view::npos) return false;
  int length = 0;
  const std::string_view lengthText = prefix.substr(slash + 1);
  if (lengthText.empty() || lengthText.size() > 3) return false;
  for (const char c : lengthText) {
    if (c < '0' || c > '9') return false;
    length = length * 10 + (c - '0');
  }
  if (length > 128) return false;
  Ipv6Bytes network;
  Ipv6Bytes candidate;
  if (!ParseIpv6Literal(prefix.substr(0, slash), &network)) return false;
  if (!ParseIpv6Literal(address, &candidate)) return false;
  int remaining = length;
  for (int i = 0; i < 16 && remaining > 0; ++i) {
    const int bits = remaining >= 8 ? 8 : remaining;
    const unsigned char mask = static_cast<unsigned char>(0xff << (8 - bits));
    if ((network.b[i] & mask) != (candidate.b[i] & mask)) return false;
    remaining -= bits;
  }
  return true;
}

// Whether `address` is a unique local address (fc00::/7), which is what the
// tunnel's own v6 address must be: private, so nothing on the internet can be
// confused with it, and outside the capture set, so the tun's own /64 never
// competes with a capture route.
constexpr bool IsIpv6UniqueLocal(std::string_view address) {
  return Ipv6PrefixContains("fc00::/7", address);
}

// ---- the dual-stack configuration floor ------------------------------------

// The v4 half: a dotted-quad address with a prefix that cannot swallow the
// default route (/0 on the tun means a CONNECTED route covering the whole
// space, captured without any of the policy routing that is supposed to decide
// it), and every v4 resolver a dotted quad. TunnelConfig owns std::string and
// std::vector values; their string-to-string_view path is runtime-only under
// the project's C++17/libstdc++ contract, so the config validators must not be
// constexpr. The literal parsers they call remain constexpr above.
inline bool IsIpv4HalfValid(const TunnelConfig& config) {
  if (!IsIpv4Literal(config.local_addr_v4) || config.prefix_v4 < 1 || config.prefix_v4 > 32) {
    return false;
  }
  for (const auto& server : config.dns_servers_v4) {
    if (!IsIpv4Literal(server)) return false;
  }
  return true;
}

// The v6 half: a ULA address (never "::", never multicast, never a global
// address the tun would then claim) with a prefix of at least /1 for the same
// reason as v4, and every v6 resolver a v6 literal. An empty resolver list is
// legal for either half: a tunnel without resolvers of that family is a tunnel
// whose DNS the other family carries, which the nft DNS floor and
// status.dns_detail both describe.
inline bool IsIpv6HalfValid(const TunnelConfig& config) {
  if (!IsIpv6UniqueLocal(config.local_addr_v6) || config.prefix_v6 < 1 ||
      config.prefix_v6 > 128) {
    return false;
  }
  for (const auto& server : config.dns_servers_v6) {
    if (!IsIpv6Literal(server)) return false;
  }
  return true;
}

// THE FLOOR. Both halves, or nothing: a configuration that can only carry one
// family is refused before the device is opened, so the tunnel never comes up
// as a split tunnel by accident.
inline bool IsDualStackTunnelConfig(const TunnelConfig& config) {
  return IsIpv4HalfValid(config) && IsIpv6HalfValid(config);
}

// ---- the v6 capture set ----------------------------------------------------

// The v6 split-default capture set: the whole IPv6 space MINUS the three
// prefixes a tunnel cannot carry — link-local fe80::/10 (NDP, RS/RA, the link
// itself), ULA fc00::/7 (the LAN analogue of RFC1918, and the family the tun's
// own address lives in) and multicast ff00::/8 (MLD, solicited-node DAD; a tun
// has no link to multicast on). The same shape Android's excludeRoute set,
// iOS's NEIPv6Settings.excludedRoutes and the Windows NetworkConfig use.
//
// Loopback ::1 is NOT excluded by route, and does not need to be: the kernel's
// `local` table (rule pref 0) claims every local address before the capture
// rule at pref 32763 is consulted — exactly why the v4 set can carry
// 127.0.0.0/8 inside 64.0.0.0/2 without capturing loopback.
inline const std::vector<std::string>& CaptureV6Prefixes() {
  static const std::vector<std::string> kPrefixes = {
      "::/1",     "8000::/2",  "c000::/3",  "e000::/4",
      "f000::/5", "f800::/6",  "fe00::/9",  "fec0::/10",
  };
  return kPrefixes;
}

// The three prefixes CaptureV6Prefixes deliberately omits, as one list, so the
// routes and the firewall's v6 permits are built from ONE table.
inline const std::vector<std::string>& ExcludedV6Prefixes() {
  static const std::vector<std::string> kPrefixes = {"fe80::/10", "fc00::/7", "ff00::/8"};
  return kPrefixes;
}

// Whether the capture set routes `address` into the tunnel: inside some
// capture prefix and inside no excluded one. Pure, for the tests.
constexpr bool CaptureV6Claims(std::string_view address) {
  constexpr std::string_view kExcluded[] = {"fe80::/10", "fc00::/7", "ff00::/8"};
  for (const auto excluded : kExcluded) {
    if (Ipv6PrefixContains(excluded, address)) return false;
  }
  constexpr std::string_view kCapture[] = {"::/1",     "8000::/2", "c000::/3", "e000::/4",
                                           "f000::/5", "f800::/6", "fe00::/9", "fec0::/10"};
  for (const auto prefix : kCapture) {
    if (Ipv6PrefixContains(prefix, address)) return true;
  }
  return false;
}

// ---- the egress witness's treatment leg ------------------------------------
//
// Tunnel::VerifyEgressWitness connect()s a socket created exactly as the SDK's
// are (the TREATMENT) right after an unmarked control socket proved the capture
// policy in force, once per family. Everything the leg decides from that one
// socket is decided here, so every outcome is a table test.
//
// ONE OUTCOME IS NEITHER A PASS NOR A FAULT: a socket the kernel could not route
// anywhere. connect() answers ENETUNREACH because this family has no physical
// uplink right now -- a network switch in progress, an IPv6-only network for the
// v4 half, an IPv4-only ISP for the v6 half. That is not a leak. An unmarked
// socket would have resolved the capture table and bound the tun (the control
// leg has just proven it), so a socket with no route at all is one that did not
// resolve that table, and cannot enter it. It proves that only when the socket
// carries the egress mark: without the mark nothing this daemon installed
// explains the escape, which is the same contradiction as a socket that bound a
// physical address unmarked. The v4 half used to fail on it outright, so a
// machine that spent a minute between networks, or joined an IPv6-only one, had
// its session torn down as "unprotected" two re-checks later.
enum class WitnessTreatment {
  // Bound an off-tunnel source while carrying the mark: steered around it.
  Escaped,
  // No route off the tunnel for this family, carrying the mark: it cannot
  // enter the tunnel either.
  Unroutable,
  // Bound the tun's own address: the capture table claimed the daemon.
  Captured,
  // Escaped, or found no route, WITHOUT the mark: contradictory evidence.
  Unmarked,
  // Anything else: this family's egress path cannot be characterised.
  Failed,
};

// The errnos with which connect() says "no route for this family": none at all
// (ENETUNREACH), an unreachable one (EHOSTUNREACH), or a route with no source
// address left to use (EADDRNOTAVAIL: still in DAD, or removed mid-switch).
constexpr bool IsNoRouteErrno(int error) {
  return error == ENETUNREACH || error == EHOSTUNREACH || error == EADDRNOTAVAIL;
}

// `bound`: connect() and getsockname() both succeeded, and `source` is the
// address the kernel bound. `connectErrno`: why connect() failed (0 when it did
// not get that far). `markKnown` and `mark`: SO_MARK as the socket was born
// with it, read before connect(), so it is known on a failed connect() too.
constexpr WitnessTreatment WitnessTreatmentVerdict(bool bound, std::string_view source,
                                                   int connectErrno, bool markKnown,
                                                   std::uint32_t mark, std::uint32_t egressMark,
                                                   std::string_view tunnelAddress) {
  const bool marked = markKnown && mark == egressMark;
  if (!bound) {
    if (!IsNoRouteErrno(connectErrno)) return WitnessTreatment::Failed;
    return marked ? WitnessTreatment::Unroutable : WitnessTreatment::Unmarked;
  }
  // Before the mark, as the measurement has always read: a socket bound to the
  // tun is captured whatever it carries.
  if (source == tunnelAddress) return WitnessTreatment::Captured;
  return marked ? WitnessTreatment::Escaped : WitnessTreatment::Unmarked;
}

// ---- a marked v6 socket on the tunnel's own address -------------------------
//
// Bound to the tun's own address is what the treatment leg calls Captured, and
// a socket with no mark so bound has resolved the capture table: nothing else
// to ask. A MARKED one has one more reading, on one host shape: an IPv6 default
// route (often learned from a router advertisement) and no usable global
// address anywhere off the tunnel. The mark passes the capture rule and main's
// default route takes the socket, so its route leaves by that interface; source
// selection runs afterwards, over every interface's addresses, and the kernel
// scores a ULA as scope global. The tun's ULA therefore beats the interface's
// own link-local address, and the socket is bound to the tunnel's address with
// a route that does not go near the tunnel. Its packets leave with a source
// nothing can answer: IPv6 has no way off this host that could need protecting.
//
// The socket alone cannot tell that from a daemon really steered into its own
// tunnel, by a rule read before the capture rule or a route through the tun in
// the table the mark leads to: all of them read mark present, source the tun's.
// So the leg asks the kernel for the route of this mark and for the addresses
// the host holds. Measured on kernel 7.0 in a network namespace:
//
//   default via fe80::1 dev wlan0, no global address
//     marked socket binds fd00::2; route get: dev wlan0 src fd00::2
//   the same with `ip -6 route add default dev urnet0 metric 100` or
//   `ip -6 rule add fwmark <mark> table 51821`: the true capture
//     marked socket binds fd00::2; route get: dev urnet0 src fd00::2
//
// `ip route get` is a hypothetical about no socket, which is why it is never the
// measurement: it only explains a socket already measured, and an answer naming
// another source describes something else, so it explains nothing. Anything
// that cannot be read refuses, as the leg always has. And it is asked again at
// every check of the witness, so a global address that appears mid-session
// turns this reading into a socket that binds that address (Escaped) or into a
// refusal, with no state kept here to go stale.
enum class CapturedV6 {
  // Marked, its route leaves by another interface, and no interface off the
  // tunnel holds a usable global address: no IPv6 egress to protect, and the
  // socket cannot enter the tunnel. The only reading that does not refuse.
  NoIpv6Egress,
  // No mark, or none that could be read: the capture table claimed it.
  Unmarked,
  // Marked, and the kernel routes it through the tun itself.
  RoutedIntoTunnel,
  // Marked, routed off the tunnel, on the tunnel's address although
  // `global_interface` holds a usable global address it could have taken.
  GlobalAddressPresent,
  // Marked, and the route or the addresses could not be read, or the route's
  // source is not the socket's: nothing explains the socket.
  Unexplained,
};

// What the leg read from the kernel for that socket. `*_ok`: the command ran
// and exited 0.
struct Ipv6HostReads {
  // `ip -o -6 addr show scope global`
  bool addresses_ok = false;
  std::string addresses;
  // `ip -6 route get <the witness's address> mark <the egress mark>`
  bool route_ok = false;
  std::string route;
};

struct CapturedV6Reading {
  CapturedV6 verdict = CapturedV6::Unexplained;
  // The interface the route leaves by, when it was read.
  std::string route_device;
  // CapturedV6::GlobalAddressPresent's interface.
  std::string global_interface;
};

namespace tunnelpolicy_detail {

inline std::vector<std::string_view> SplitWords(std::string_view text) {
  std::vector<std::string_view> words;
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r')) ++at;
    std::size_t end = at;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\r') ++end;
    if (end > at) words.push_back(text.substr(at, end - at));
    at = end;
  }
  return words;
}

}  // namespace tunnelpolicy_detail

// Whether the flags on one line of `ip -o addr` leave the address a source the
// kernel gives a new socket. Tentative and failed-DAD addresses are no candidate
// at all; deprecated and optimistic ones rank below a healthy ULA (measured for
// deprecated, kernel 7.0: the marked socket binds the tun's ULA over it), so
// neither gets a new socket off this host while the tunnel is up.
inline bool AddressLineIsUsableSource(std::string_view line) {
  for (const std::string_view word : tunnelpolicy_detail::SplitWords(line)) {
    if (word == "tentative" || word == "dadfailed" || word == "deprecated" ||
        word == "optimistic") {
      return false;
    }
  }
  return true;
}

// The first interface other than `tunnelInterface` holding a usable global IPv6
// address in `listing` (`ip -o -6 addr show scope global`, one address a line:
// "<index>: <name> inet6 <address>/<length> ..."), "" when none does, nullopt
// when a line cannot be read: this answer is what lets a tunnel come up, so
// nothing is assumed about what it could not read. A ULA is scope global to the
// kernel but no source anything beyond its site can answer, so it does not count.
inline std::optional<std::string> UsableGlobalIpv6Interface(std::string_view listing,
                                                            std::string_view tunnelInterface) {
  std::size_t at = 0;
  while (at < listing.size()) {
    std::size_t end = listing.find('\n', at);
    if (end == std::string_view::npos) end = listing.size();
    const std::string_view line = listing.substr(at, end - at);
    at = end + 1;
    const std::vector<std::string_view> words = tunnelpolicy_detail::SplitWords(line);
    if (words.empty()) continue;
    if (words.size() < 4 || words[0].back() != ':' || words[2] != "inet6") return std::nullopt;
    const std::string_view address = words[3].substr(0, words[3].find('/'));
    if (!IsIpv6Literal(address)) return std::nullopt;
    const std::string_view name = words[1].substr(0, words[1].find('@'));  // eth0.10@eth0
    if (name == tunnelInterface || !AddressLineIsUsableSource(line)) continue;
    if (!IsIpv6UniqueLocal(address)) return std::string(name);
  }
  return std::string();
}

// The word after `key` on the first line of `ip route get`'s answer ("<dest> from
// :: via <gateway> dev <name> proto ra src <source> metric 600 pref medium"), ""
// when it is not there.
inline std::string RouteGetField(std::string_view answer, std::string_view key) {
  const std::vector<std::string_view> words =
      tunnelpolicy_detail::SplitWords(answer.substr(0, answer.find('\n')));
  for (std::size_t i = 0; i + 1 < words.size(); ++i) {
    if (words[i] == key) return std::string(words[i + 1]);
  }
  return std::string();
}

// `marked`: the socket reads the egress mark. `source`: the address it bound,
// the tunnel's. `tunnelInterface`: the tun's name.
inline CapturedV6Reading ReadCapturedV6(bool marked, std::string_view source,
                                        std::string_view tunnelInterface,
                                        const Ipv6HostReads& reads) {
  CapturedV6Reading reading;
  if (!marked) {
    reading.verdict = CapturedV6::Unmarked;
    return reading;
  }
  if (!reads.route_ok) return reading;
  reading.route_device = RouteGetField(reads.route, "dev");
  if (reading.route_device.empty() || RouteGetField(reads.route, "src") != source) return reading;
  if (reading.route_device == tunnelInterface) {
    reading.verdict = CapturedV6::RoutedIntoTunnel;
    return reading;
  }
  // lo is where an unreachable or a local answer "leaves": no way off anything.
  if (reading.route_device == "lo" || !reads.addresses_ok) return reading;
  const std::optional<std::string> global =
      UsableGlobalIpv6Interface(reads.addresses, tunnelInterface);
  if (!global) return reading;
  reading.global_interface = *global;
  reading.verdict = global->empty() ? CapturedV6::NoIpv6Egress : CapturedV6::GlobalAddressPresent;
  return reading;
}

// Why a v6 socket on the tunnel's address refuses, for every reading but
// NoIpv6Egress. Only Unmarked names the mark as the fault: the others have read
// it on the socket, and sending someone to debug a mark that works is how this
// refusal misled the host it was first reported on.
inline std::string CapturedV6RefusalReason(const CapturedV6Reading& reading,
                                           const std::string& source,
                                           const std::string& tunnelInterface) {
  const std::string head =
      "a v6 socket this daemon just opened was bound to the tunnel's own address (" + source + ")";
  switch (reading.verdict) {
    case CapturedV6::Unmarked:
      return head +
             ", so it resolved the capture table before anything could steer it away. The mark "
             "is not reaching this process's IPv6 sockets at creation time";
    case CapturedV6::RoutedIntoTunnel:
      return head + " and the kernel routes it through " + tunnelInterface +
             ". It carries the mark, so the mark is working: a policy rule read before the "
             "capture rule, or a route through the tunnel in the table the mark leads to, is "
             "sending it back in";
    case CapturedV6::GlobalAddressPresent:
      return head + " while " + reading.global_interface +
             " holds a usable global IPv6 address. It carries the mark and its route leaves by " +
             reading.route_device +
             ", so the mark is working, but a socket so bound can never be answered and nothing "
             "explains it";
    case CapturedV6::NoIpv6Egress:
    case CapturedV6::Unexplained:
      break;
  }
  return head +
         ". It carries the mark, so the mark is working, but its route and this host's IPv6 "
         "addresses could not be read as agreeing with it, so it cannot be shown to stay out of "
         "the tunnel";
}

// The v4 half has no reading but the refusal; this is only its sentence, which
// must not blame a mark the socket carries either.
inline std::string CapturedV4RefusalReason(bool marked, const std::string& source) {
  const std::string head =
      "a socket this daemon just opened was bound to the tunnel's own address (" + source + ")";
  if (marked) {
    return head +
           ". It carries the mark, so the mark is working: a policy rule read before the capture "
           "rule, a route through the tunnel in the table the mark leads to, or an interface "
           "with no address of its own took it there";
  }
  return head +
         ", so it resolved the capture table before anything could steer it away. The mark is "
         "not reaching this process's sockets at creation time, and no mark applied after "
         "connect() can undo it";
}

// THE R4 REFUSAL'S TEXT, for every leg of the witness: what failed, and the
// command that reports the host state behind it. `self` is the running
// binary's own path (daemon/SelfPath.hpp). No package puts urnetworkd on
// $PATH, so the bare `urnetworkd --diagnose` this used to name was "command
// not found" for the person reading it, in the app as in the journal.
inline std::string EgressRefusalMessage(const std::string& reason, const std::string& self) {
  return "this daemon cannot prove its own traffic stays out of its own tunnel, so the "
         "tunnel is not being run: " +
         reason + ". `" + self +
         " --diagnose` needs no root and reports the host preflight and the policy rules, "
         "routes and addresses this daemon's own sockets meet, IPv4 and IPv6; "
         "packaging/diagnose-egress.sh in the source tree goes deeper. The last time this "
         "went unproven the daemon looped 3.38 Tb of its own traffic through its own tunnel "
         "before a human noticed.";
}

// ---- the direct-file DNS tier: re-applying over a rewrite ------------------
//
// Tier 3 owns /etc/resolv.conf and keeps what was there before in a backup for
// the disconnect to put back. NetworkManager's dns=default plugin and dhcpcd
// rewrite the file at every network switch, and the reaper puts the tunnel's
// file back over theirs (TunnelHost::MaintainDnsLocked). That re-apply used to
// keep the backup taken at bring-up, which defeated the restore's own guard
// against putting back "a stale copy of a state the machine has already left":
// once the reaper had re-applied, the marker was ours again, so a disconnect
// after a network switch restored the PREVIOUS network's resolvers.
//
// What the re-apply finds decides it. Our own file (it carries the marker) is
// rewritten with the backup untouched: backing up our own file is the
// double-takeover bug. Someone else's file naming a real resolver is the newest
// truth about this machine's DNS and becomes the backup. Someone else's file
// naming no resolver at all (a writer caught mid-write, an emptied file), or
// naming one of the tunnel's own resolvers (a copy of ours), is never what a
// disconnect may put back, so the older backup stays.
enum class ResolvConfReapply {
  KeepBackup,
  AdoptAsBackup,
};

// The `nameserver` addresses in resolv.conf content, in order, read the way
// glibc reads them: the keyword in the first column, a space or tab, then the
// address, with anything after it ignored. Comment lines (`#`, `;`) and a
// keyword with no separator ("nameserver10.0.0.1", which glibc ignores) are not
// nameservers.
inline std::vector<std::string> ResolvConfNameservers(std::string_view content) {
  constexpr std::string_view kKeyword = "nameserver";
  std::vector<std::string> servers;
  for (std::size_t at = 0; at < content.size();) {
    std::size_t end = content.find('\n', at);
    if (end == std::string_view::npos) end = content.size();
    const std::string_view line = content.substr(at, end - at);
    at = end + 1;
    if (line.substr(0, kKeyword.size()) != kKeyword) continue;
    std::size_t pos = kKeyword.size();
    if (pos >= line.size() || (line[pos] != ' ' && line[pos] != '\t')) continue;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    std::size_t stop = pos;
    while (stop < line.size() && line[stop] != ' ' && line[stop] != '\t' && line[stop] != '\r') {
      ++stop;
    }
    if (stop > pos) servers.emplace_back(line.substr(pos, stop - pos));
  }
  return servers;
}

// `current`: what /etc/resolv.conf holds now. `ownMarker`: the marker line in
// every file tier 3 writes. `tunnelResolvers`: the resolvers those files name.
inline ResolvConfReapply DecideResolvConfReapply(std::string_view current,
                                                 std::string_view ownMarker,
                                                 const std::vector<std::string>& tunnelResolvers) {
  if (!ownMarker.empty() && current.find(ownMarker) != std::string_view::npos) {
    return ResolvConfReapply::KeepBackup;
  }
  const std::vector<std::string> servers = ResolvConfNameservers(current);
  if (servers.empty()) return ResolvConfReapply::KeepBackup;
  for (const auto& server : servers) {
    for (const auto& own : tunnelResolvers) {
      if (server == own) return ResolvConfReapply::KeepBackup;
    }
  }
  return ResolvConfReapply::AdoptAsBackup;
}

// ---- per-app split tunnel: urnetwork-exclude --------------------------------
//
// `urnetwork-exclude <command>` (app/packaging/urnetwork-exclude) runs a
// command in the user's systemd slice urnetwork-exclude.slice. While a ruleset
// is installed, urnetworkd lets the sockets in the tunnel owner's slice leave
// outside the tunnel, and nothing else:
//   * urnw_mark_out marks their packets with kBypassMark (`socket cgroupv2`,
//     the same match the daemon uses for its own sockets);
//   * one policy rule per family sends that mark to the main table, ahead of
//     the capture rule;
//   * urnw_nat masquerades it on the physical interface, because connect()
//     chose the tun's source address before the mark existed (Tunnel.hpp,
//     EgressSocketMarker: a mark set at the output hook re-routes the packet
//     but keeps the source address);
//   * urnw_out accepts it, so neither the kill-switch floor nor the off-tunnel
//     IPv6 and DNS blocks apply to it. The metadata-address drop still does.
// Only the owner's slice: another local user excluding their processes would
// bypass a tunnel and a kill switch they do not control, so their commands stay
// in the tunnel. A host that is not on the cgroup v2 unified hierarchy has no
// slice to match; the launcher refuses there and the daemon emits none of this.

// "URNX", the egress mark's neighbour, set only by the exclusion rule. Like
// kEgressMark it can be forged with CAP_NET_ADMIN, which here already means
// root.
inline constexpr std::uint32_t kBypassMark = 0x55524e58u;

// Above the capture rule (kFwmarkRulePriority, 32763) so the mark is decided
// first, and not 32762 (kSuppressRulePriority), the retired rule the sweeps
// still delete.
inline constexpr int kBypassRulePriority = 32761;

// A mark as nft and ip print it ("0x%08x").
inline std::string MarkText(std::uint32_t mark) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text = "0x00000000";
  for (int i = 0; i < 8; ++i) text[9 - i] = kHex[(mark >> (4 * i)) & 0xfu];
  return text;
}

// The slice `systemd-run --user --slice=urnetwork-exclude.slice` creates, as a
// path below /sys/fs/cgroup (no leading slash, like CgroupRef::path). The dash
// makes it a child of urnetwork.slice (systemd's slice naming). "" for a
// negative uid, which is "nobody owns the tunnel".
inline std::string ExcludeSliceCgroupPath(std::int64_t uid) {
  if (uid < 0) return std::string();
  const std::string id = std::to_string(uid);
  return "user.slice/user-" + id + ".slice/user@" + id +
         ".service/urnetwork.slice/urnetwork-exclude.slice";
}

// nft's `socket cgroupv2 level N`: the component count of a cgroup path.
inline int CgroupPathLevel(std::string_view path) {
  if (path.empty()) return 0;
  int level = 1;
  for (const char c : path) {
    if (c == '/') ++level;
  }
  return level;
}

// Whether /proc/self/cgroup describes the cgroup v2 unified hierarchy alone:
// "0::<path>" lines and no v1 hierarchy. A legacy (v1-only) host has no 0::
// line; a hybrid host has both, and there /sys/fs/cgroup holds the v1
// controllers, so no slice path resolves where nft and the launcher look for
// it. Either way per-app exclusion is hidden.
inline bool IsCgroupV2Only(std::string_view procSelfCgroup) {
  bool unified = false;
  for (std::size_t at = 0; at < procSelfCgroup.size();) {
    std::size_t end = procSelfCgroup.find('\n', at);
    if (end == std::string_view::npos) end = procSelfCgroup.size();
    const std::string_view line = procSelfCgroup.substr(at, end - at);
    at = end + 1;
    if (line.empty()) continue;
    if (line.substr(0, 3) != "0::") return false;
    unified = true;
  }
  return unified;
}

// urnw_mark_out: mark the slice's packets. No verdict, like the daemon's rule.
inline std::string ExcludeMarkRule(std::string_view cgroupPath, int level) {
  return "socket cgroupv2 level " + std::to_string(level) + " \"" + std::string(cgroupPath) +
         "\" counter meta mark set " + MarkText(kBypassMark);
}

// urnw_out: a marked packet leaves, floor or not.
inline std::string ExcludeAcceptRule() {
  return "meta mark " + MarkText(kBypassMark) + " counter accept";
}

// urnw_nat: replace the tun source address connect() chose with the physical
// interface's. Never on the tun itself or on loopback.
inline std::string ExcludeMasqueradeRule(std::string_view tunName) {
  return "meta mark " + MarkText(kBypassMark) + " oifname != \"" + std::string(tunName) +
         "\" oifname != \"lo\" counter masquerade";
}

// The policy rule's selector, the same for `ip -4|-6 rule add` and `delete`,
// so a delete names exactly the rule an add installed and nothing that merely
// shares its priority.
inline std::vector<std::string> BypassRuleSelector() {
  return {"fwmark", MarkText(kBypassMark), "table", "main",
          "pref",   std::to_string(kBypassRulePriority)};
}

}  // namespace urnw
