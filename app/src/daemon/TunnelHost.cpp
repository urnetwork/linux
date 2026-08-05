// SPDX-License-Identifier: MPL-2.0
#include "TunnelHost.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "NetworkSpaceConfig.hpp"

namespace urnw {
namespace {

// Persisted device identity, file-per-part like the Windows TunnelController
// (client_key_seed.bin / provide_cert.pem / provide_key.pem).
constexpr const char* kClientKeySeedFile = "client_key_seed.bin";
constexpr const char* kProvideCertFile = "provide_cert.pem";
constexpr const char* kProvideKeyFile = "provide_key.pem";

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}

void WriteFileBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::fprintf(stderr, "[tunnel] could not write %s\n", path.c_str());
      return;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  }
  // key material: owner-only, on top of the root-owned 0700 state dir
  ::chmod(path.c_str(), 0600);
}

}  // namespace

TunnelHost::TunnelHost(std::string storageRoot) : storageRoot_(std::move(storageRoot)) {}

TunnelHost::~TunnelHost() { Stop(); }

std::optional<urnet::DeviceLocalKeyMaterial> TunnelHost::LoadKeyMaterial() const {
  const auto seed = ReadFileBytes(storageRoot_ + "/" + kClientKeySeedFile);
  const auto cert = ReadFileBytes(storageRoot_ + "/" + kProvideCertFile);
  const auto key = ReadFileBytes(storageRoot_ + "/" + kProvideKeyFile);
  if (seed.empty() || cert.empty() || key.empty()) return std::nullopt;
  return urnet::newDeviceLocalKeyMaterial(
      seed.data(), static_cast<int32_t>(seed.size()), cert.data(),
      static_cast<int32_t>(cert.size()), key.data(), static_cast<int32_t>(key.size()));
}

void TunnelHost::PersistKeyMaterial(const urnet::DeviceLocalKeyMaterial& km) const {
  WriteFileBytes(storageRoot_ + "/" + kClientKeySeedFile, km.getClientKeySeed());
  WriteFileBytes(storageRoot_ + "/" + kProvideCertFile, km.getProvideTlsCertificatePem());
  WriteFileBytes(storageRoot_ + "/" + kProvideKeyFile, km.getProvideTlsPrivateKeyPem());
}

ctl::StatusReply TunnelHost::Start(const ctl::StartTunnelRequest& config) {
  StopInternal();  // idempotent restart
  state_ = ctl::TunnelState::Starting;
  error_.clear();

  try {
    // --- network space (daemon-owned storage; same values as the GUI's) ---
    if (!spaceManager_) {
      spaceManager_ = urnet::newNetworkSpaceManager(storageRoot_ + "/sdk");
    }
    networkSpace_ = BuildUrNetworkSpace(*spaceManager_);

    // --- DeviceLocal, rpc enabled: the SDK starts the loopback mTLS RPC
    //     listener (127.0.0.1:12025) the GUI's DeviceRemote dials. Stable
    //     provider identity via the persisted key material; unusable stored
    //     material falls back to a fresh identity, which is persisted
    //     immediately (nothing event-driven re-saves it). ---
    const std::string appVersion =
        config.app_version.empty() ? kUrAppVersionFallback : config.app_version;
    if (auto km = LoadKeyMaterial()) {
      try {
        device_ = urnet::newDeviceLocalWithKeyMaterial(
            *networkSpace_, config.by_jwt, UrDeviceDescription(), UrDeviceSpec(), appVersion,
            config.instance_id, /*enable_rpc=*/true, *km);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "[tunnel] restore device key material failed: %s\n", e.what());
      }
    }
    if (!device_) {
      device_ = urnet::newDeviceLocalWithDefaults(*networkSpace_, config.by_jwt,
                                                  UrDeviceDescription(), UrDeviceSpec(),
                                                  appVersion, config.instance_id,
                                                  /*enable_rpc=*/true);
      try {
        if (auto km = device_->getKeyMaterial(); km && !km.isEmpty()) PersistKeyMaterial(km);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "[tunnel] persist device key material failed: %s\n", e.what());
      }
    }

    // A set_provide that arrived while the tunnel was down applies now. (The
    // GUI also restores its persisted mode over the device RPC right after
    // start; the last writer wins, and both come from the same stored value.)
    if (!pendingProvideMode_.empty()) {
      device_->setProvideControlMode(pendingProvideMode_);
    }

    // --- tun (address/dns from the device, exactly the GUI's old logic) ---
    TunnelConfig cfg;
    cfg.local_addr = device_->tunnelLocalAddress();
    if (cfg.local_addr.empty()) cfg.local_addr = "169.254.2.1";
    // dns from the device: the dns settings' unencrypted local servers when
    // set, otherwise the distinct plain-DNS UpgradeMux mask. Always plain :53,
    // never OS-level encrypted DNS: the mux performs the unencrypted-DNS ->
    // DoH upgrade in-tunnel. The tunnel is ipv4-only.
    if (auto dns = device_->tunnelDnsAddressesIpv4(); dns && !dns->empty()) {
      cfg.dns_servers = *dns;
    } else {
      // Keep the exceptional fallback coupled to the SDK's separately tested
      // URnetwork-owned UpgradeMux identity.
      cfg.dns_servers = {urnet::getDefaultTunnelDnsAddressIpv4()};
    }

    tunnel_ = Tunnel::Open(cfg);
    if (!tunnel_) throw std::runtime_error("could not open/configure the tun device");

    // hand the tun fd to the SDK's fd loop (the Android/Linux data plane path)
    ioLoop_ = urnet::newIoLoop(*device_, tunnel_->fd(), [] {
      // The loop exits when the fd or device closes — ordinarily our own
      // Stop(). Anything else is logged; the next start_tunnel rebuilds.
      std::fprintf(stderr, "[tunnel] io loop finished\n");
    });
    device_->setTunnelStarted(true);

    state_ = ctl::TunnelState::Up;
    std::fprintf(stderr, "[tunnel] up (client=%s rpc=127.0.0.1:%d)\n",
                 device_->getClientId().c_str(), ctl::kDeviceRpcPort);
  } catch (const std::exception& e) {
    error_ = e.what();
    std::fprintf(stderr, "[tunnel] start failed: %s\n", error_.c_str());
    // Retryable: tear down every partially-created resource so a failed
    // attempt cannot leave an IoLoop or a half-configured tun behind.
    StopInternal();
    state_ = ctl::TunnelState::Error;
  }
  return Status();
}

void TunnelHost::Stop() { StopInternal(); }

void TunnelHost::StopInternal() {
  if (state_ == ctl::TunnelState::Up || state_ == ctl::TunnelState::Starting) {
    state_ = ctl::TunnelState::Stopping;
  }
  // Reverse dependency order. close() actually stops the SDK goroutines and
  // the IoLoop; releasing the handle alone would leak them.
  if (device_) device_->setTunnelStarted(false);
  if (ioLoop_) ioLoop_->close();
  ioLoop_.reset();
  tunnel_.reset();  // closes the fd: the non-persistent tun + routes vanish
  if (device_) {
    device_->close();
    device_.reset();
  }
  // spaceManager_/networkSpace_ persist across sessions (Windows parity).
  state_ = ctl::TunnelState::Stopped;
}

bool TunnelHost::SetProvideMode(const std::string& mode) {
  pendingProvideMode_ = mode;  // survives a stop/start cycle
  if (device_) {
    try {
      device_->setProvideControlMode(mode);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[tunnel] set provide mode failed: %s\n", e.what());
      return false;
    }
  }
  return true;
}

ctl::StatusReply TunnelHost::Status() const {
  ctl::StatusReply s;
  s.tunnel_state = state_;
  s.rpc_port = (state_ == ctl::TunnelState::Up) ? ctl::kDeviceRpcPort : 0;
  if (device_) {
    try {
      s.client_id = device_->getClientId();
    } catch (const std::exception&) {
      // status must never throw across the wire
    }
  }
  s.error = error_;
  return s;
}

}  // namespace urnw
