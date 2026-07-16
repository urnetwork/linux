// SPDX-License-Identifier: MPL-2.0
#include "Tunnel.hpp"

#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
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
  const std::array<std::vector<std::string>, 5> steps = {{
      {"ip", "link", "set", "dev", name_, "up"},
      {"ip", "link", "set", "dev", name_, "mtu", std::to_string(cfg.mtu)},
      {"ip", "address", "add", addr, "dev", name_},
      // split-default capture: 0.0.0.0/1 + 128.0.0.0/1 sort above the physical
      // default route without deleting it (the macOS/wg-quick trick).
      {"ip", "route", "add", "0.0.0.0/1", "dev", name_},
      {"ip", "route", "add", "128.0.0.0/1", "dev", name_},
  }};
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
