// SPDX-License-Identifier: MPL-2.0
#include "Tunnel.hpp"

#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace urnw {
namespace {

// Run a command, returning true on exit 0. Best-effort logging to stderr.
bool Run(const std::vector<std::string>& args) {
  std::string cmd;
  for (const auto& a : args) {
    if (!cmd.empty()) cmd += ' ';
    cmd += a;
  }
  int rc = std::system(cmd.c_str());
  if (rc != 0) std::fprintf(stderr, "[tun] `%s` exited %d\n", cmd.c_str(), rc);
  return rc == 0;
}

}  // namespace

std::unique_ptr<Tunnel> Tunnel::Open(const TunnelConfig& cfg) {
  int fd = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    std::perror("[tun] open /dev/net/tun (need CAP_NET_ADMIN)");
    return nullptr;
  }

  ifreq ifr{};
  // IFF_TUN: layer-3 IP packets; IFF_NO_PI: no 4-byte protocol-info prefix, so
  // the fd carries raw IP packets exactly as the SDK IoLoop expects.
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  std::strncpy(ifr.ifr_name, cfg.name.c_str(), IFNAMSIZ - 1);
  if (::ioctl(fd, TUNSETIFF, &ifr) < 0) {
    std::perror("[tun] TUNSETIFF");
    ::close(fd);
    return nullptr;
  }

  auto t = std::unique_ptr<Tunnel>(new Tunnel());
  t->fd_ = fd;
  t->name_ = ifr.ifr_name;  // the actual assigned name
  if (!t->Configure(cfg)) {
    return nullptr;  // dtor closes the fd + tears down
  }
  std::string dnsList;
  for (const auto& dns : cfg.dns_servers) {
    if (!dnsList.empty()) dnsList += ',';
    dnsList += dns;
  }
  std::fprintf(stderr, "[tun] up %s addr=%s/%d mtu=%d dns=%s\n", t->name_.c_str(),
               cfg.local_addr.c_str(), cfg.prefix, cfg.mtu, dnsList.c_str());
  return t;
}

bool Tunnel::Configure(const TunnelConfig& cfg) {
  const std::string addr = cfg.local_addr + "/" + std::to_string(cfg.prefix);
  std::vector<std::vector<std::string>> steps = {
      {"ip", "link", "set", "dev", name_, "up"},
      {"ip", "link", "set", "dev", name_, "mtu", std::to_string(cfg.mtu)},
      {"ip", "address", "add", addr, "dev", name_},
  };
  // Split-default capture that EXCLUDES the local network, matching Android
  // (MainService's excludeRoute set) and iOS (NEIPv4Settings.excludedRoutes): the
  // whole ipv4 space MINUS 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, so LAN
  // traffic bypasses the tunnel and reaches local devices directly. These are the
  // complement prefixes of those private ranges within 0.0.0.0/0 (the same set
  // Android adds on its no-excludeRoute path); the excluded ranges fall through to
  // the physical/connected routes. Like the old 0.0.0.0/1 + 128.0.0.0/1 capture,
  // these sort above the physical default without deleting it.
  static const char* kIncludedV4Prefixes[] = {
      "0.0.0.0/5", "8.0.0.0/7", "11.0.0.0/8", "12.0.0.0/6", "16.0.0.0/4",
      "32.0.0.0/3", "64.0.0.0/2", "128.0.0.0/3", "160.0.0.0/5", "168.0.0.0/6",
      "172.0.0.0/12", "172.32.0.0/11", "172.64.0.0/10", "172.128.0.0/9",
      "173.0.0.0/8", "174.0.0.0/7", "176.0.0.0/4", "192.0.0.0/9", "192.128.0.0/11",
      "192.160.0.0/13", "192.169.0.0/16", "192.170.0.0/15", "192.172.0.0/14",
      "192.176.0.0/12", "192.192.0.0/10", "193.0.0.0/8", "194.0.0.0/7",
      "196.0.0.0/6", "200.0.0.0/5", "208.0.0.0/4", "224.0.0.0/3",
  };
  for (const char* prefix : kIncludedV4Prefixes) {
    steps.push_back({"ip", "route", "add", prefix, "dev", name_});
  }
  for (const auto& s : steps) {
    if (!Run(s)) return false;
  }
  // DNS via systemd-resolved: this link's resolvers + route all queries through
  // it (~. is resolved's "default route" domain). Best-effort (no resolved -> skip).
  if (!cfg.dns_servers.empty()) {
    std::vector<std::string> dnsCmd = {"resolvectl", "dns", name_};
    dnsCmd.insert(dnsCmd.end(), cfg.dns_servers.begin(), cfg.dns_servers.end());
    Run(dnsCmd);
    Run({"resolvectl", "domain", name_, "~."});
    // force plain :53 on this link: never OS-level encrypted DNS for the tunnel.
    // the UpgradeMux performs the unencrypted-DNS -> DoH upgrade in-tunnel and
    // needs to see :53, so override any global DNSOverTLS default for this link.
    Run({"resolvectl", "dnsovertls", name_, "no"});
  }
  return true;
}

Tunnel::~Tunnel() {
  if (!name_.empty()) Run({"resolvectl", "revert", name_});
  if (fd_ >= 0) {
    ::close(fd_);  // non-persistent tun: closing removes the interface + routes
    fd_ = -1;
  }
  std::fprintf(stderr, "[tun] down %s\n", name_.c_str());
}

}  // namespace urnw
