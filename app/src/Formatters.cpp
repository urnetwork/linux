// SPDX-License-Identifier: MPL-2.0
#include "Formatters.hpp"

#include "I18n.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include <arpa/inet.h>
#include <glib.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <urnetwork_sdk.hpp>

namespace urnw {
namespace {

// ">=100 -> %.0f, >=10 -> %.1f, else %.2f" magnitude formatting shared by the
// byte and bit rate helpers.
std::string FormatScaled(double value, const char* unit) {
  char buf[64];
  if (value >= 100) {
    std::snprintf(buf, sizeof(buf), "%.0f %s", value, unit);
  } else if (value >= 10) {
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, unit);
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f %s", value, unit);
  }
  return buf;
}

// Shows all values when there are 20 or fewer, else the first, middle, and
// last 7 in alphanumeric order (21 max) with the omitted count.
std::string CompactValueList(const std::vector<std::string>& values) {
  auto join = [](auto begin, auto end) {
    std::string out;
    for (auto it = begin; it != end; ++it) {
      if (!out.empty()) out += ", ";
      out += *it;
    }
    return out;
  };
  if (values.size() <= 20) {
    return join(values.begin(), values.end());
  }
  std::vector<std::string> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  const size_t n = sorted.size();
  const size_t middleStart = (n - 7) / 2;
  std::string text = join(sorted.begin(), sorted.begin() + 7) + ", …, " +
                     join(sorted.begin() + middleStart, sorted.begin() + middleStart + 7) +
                     ", …, " + join(sorted.begin() + (n - 7), sorted.end());
  const size_t omitted = n - 21;
  if (0 < omitted) {
    // plural rules live in the catalog ("plus_n_more")
    text += " " + Format(TN_("plus_n_more", "+ {} more", "+ {} more", omitted), omitted);
  }
  return text;
}

}  // namespace

std::string FormatByteCountCompact(int64_t byteCount) {
  constexpr double kKib = 1024.0;
  constexpr double kMib = kKib * 1024;
  constexpr double kGib = kMib * 1024;
  constexpr double kTib = kGib * 1024;
  const double v = static_cast<double>(byteCount);
  if (v < kKib) return std::to_string(byteCount) + " B";
  if (v < kMib) return FormatScaled(v / kKib, "KiB");
  if (v < kGib) return FormatScaled(v / kMib, "MiB");
  if (v < kTib) return FormatScaled(v / kGib, "GiB");
  return FormatScaled(v / kTib, "TiB");
}

std::string FormatByteRate(int64_t bytesPerSecond) {
  return FormatByteCountCompact(bytesPerSecond) + "/s";
}

std::string FormatCountCompact(int64_t count) {
  const double v = static_cast<double>(count);
  if (count < 1000) return std::to_string(count);
  char buf[64];
  if (v < 1e6) {
    std::snprintf(buf, sizeof(buf), v < 1e4 ? "%.1fk" : "%.0fk", v / 1000);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1fM", v / 1e6);
  }
  return buf;
}

std::string FormatPacketRate(int64_t packetsPerSecond) {
  return FormatCountCompact(packetsPerSecond) + " pkt/s";
}

std::string FormatBitRate(int64_t bitsPerSecond) {
  const double v = static_cast<double>(bitsPerSecond);
  if (v < 1e3) return std::to_string(bitsPerSecond) + " bps";
  if (v < 1e6) return FormatScaled(v / 1e3, "Kbps");
  if (v < 1e9) return FormatScaled(v / 1e6, "Mbps");
  return FormatScaled(v / 1e9, "Gbps");
}

std::string FormatHostClusterText(const std::vector<std::string>& hosts,
                                  const std::vector<std::string>& ips) {
  // host names collapse to public-suffix base names when there are more than 10
  std::vector<std::string> displayHosts = hosts;
  if (hosts.size() > 10) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> collapsed;
    for (const auto& host : hosts) {
      std::string display = "*." + urnet::hostBaseName(host);
      if (seen.insert(display).second) collapsed.push_back(std::move(display));
    }
    displayHosts = std::move(collapsed);
  }
  std::vector<std::string> items = std::move(displayHosts);
  items.insert(items.end(), ips.begin(), ips.end());
  if (items.empty()) return T_("unknown", "unknown");
  return CompactValueList(items);
}

std::string RelativeTime(int64_t secondsAgo) {
  const int64_t seconds = std::max<int64_t>(0, secondsAgo);
  if (seconds < 5) return T_("now", "now");
  if (seconds < 60) return Format(T_("seconds_ago_abbrev", "{}s ago"), seconds);
  if (seconds < 3600) return Format(T_("minutes_ago_abbrev", "{}m ago"), seconds / 60);
  return Format(T_("hours_ago_abbrev", "{}h ago"), seconds / 3600);
}

bool IsIpAddressValue(const std::string& value) {
  unsigned char buf[sizeof(struct in6_addr)];
  return inet_pton(AF_INET, value.c_str(), buf) == 1 ||
         inet_pton(AF_INET6, value.c_str(), buf) == 1;
}

bool IsValidDohUrl(const std::string& value) {
  GUri* uri = g_uri_parse(value.c_str(), G_URI_FLAGS_NONE, nullptr);
  if (!uri) return false;
  const char* scheme = g_uri_get_scheme(uri);
  const char* host = g_uri_get_host(uri);
  const bool ok = scheme && g_ascii_strcasecmp(scheme, "https") == 0 && host && *host;
  g_uri_unref(uri);
  return ok;
}

std::string TrimWhitespace(const std::string& value) {
  const char* ws = " \t\r\n";
  const size_t begin = value.find_first_not_of(ws);
  if (begin == std::string::npos) return "";
  const size_t end = value.find_last_not_of(ws);
  return value.substr(begin, end - begin + 1);
}

}  // namespace urnw
