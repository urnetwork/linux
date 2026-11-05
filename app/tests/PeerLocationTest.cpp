// What a network peer row connects to (PeerLocation.hpp): a tap on one of the
// user's own devices in the chooser or on the Network page connects to the
// peer's client id with network_peer set, so the SDK reaches the device as a
// trusted same-network peer (the Network provide mode) and not as a public
// exit, as android and apple do. The rows need GTK and the SDK, so the wiring
// cases read their source.
// SPDX-License-Identifier: MPL-2.0
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "PeerLocation.hpp"
#include "TestHarness.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

// The fields of urnet::NetworkPeer, urnet::ConnectLocationId and
// urnet::ConnectLocation, by the generated urnetwork_sdk.hpp's names and types.
struct Peer {
  std::optional<std::string> ClientId;
  bool ProvideEnabled{};
  std::string Principal{};
  std::string DeviceSpec{};
  std::string DeviceName{};
};

struct LocationId {
  std::optional<std::string> client_id;
  std::optional<std::string> location_id;
  std::optional<std::string> location_group_id;
  std::optional<bool> best_available;
};

struct Location {
  std::optional<LocationId> connect_location_id;
  std::optional<std::string> name;
  std::optional<int32_t> provider_count;
  std::optional<std::string> location_type;
  bool stable{};
  bool strong_privacy{};
  std::optional<bool> network_peer;
};

const std::string kPeerClientId = "018f2c3e-7a10-7b44-9c1d-5e2a3f4b5c6d";

Peer MakePeer(const std::string& name, const std::string& spec) {
  Peer peer;
  peer.ClientId = kPeerClientId;
  peer.ProvideEnabled = true;
  peer.DeviceName = name;
  peer.DeviceSpec = spec;
  return peer;
}

std::string ReadSource(const std::string& name) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + name, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// The definition that starts at `signature`, through the closing brace in its
// first column; empty when it is gone.
std::string DefinitionBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

size_t Count(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

UR_TEST(peerLocationIsThePeersClientIdAsANetworkPeer) {
  const Location location =
      urnw::PeerConnectLocation<Location>(MakePeer("Office laptop", "Ubuntu 24.04"));
  // without the flag the SDK reaches the device as a public exit
  UR_EXPECT_TRUE(location.network_peer == std::optional<bool>(true));
  UR_EXPECT_TRUE(location.connect_location_id.has_value());
  if (!location.connect_location_id) return;
  const LocationId& id = *location.connect_location_id;
  UR_EXPECT_TRUE(id.client_id == std::optional<std::string>(kPeerClientId));
  UR_EXPECT_FALSE(id.location_id.has_value());
  UR_EXPECT_FALSE(id.location_group_id.has_value());
  UR_EXPECT_FALSE(id.best_available.value_or(false));
  UR_EXPECT_TRUE(location.name == std::optional<std::string>("Office laptop"));
}

UR_TEST(peerDisplayNameIsTheDeviceNameThenSpecThenClientId) {
  UR_EXPECT_TRUE(urnw::PeerDisplayName(MakePeer("Office laptop", "Ubuntu 24.04")) ==
                 "Office laptop");
  UR_EXPECT_TRUE(urnw::PeerDisplayName(MakePeer("", "Ubuntu 24.04")) == "Ubuntu 24.04");
  UR_EXPECT_TRUE(urnw::PeerDisplayName(MakePeer("", "")) == kPeerClientId);
  Peer anonymous = MakePeer("", "");
  anonymous.ClientId.reset();
  UR_EXPECT_TRUE(urnw::PeerDisplayName(anonymous).empty());
  // a name made only of characters the display filter drops would show as
  // nothing, so it falls through to the next
  const std::string zeroWidth = "\xE2\x80\x8B";  // U+200B
  UR_EXPECT_TRUE(urnw::PeerDisplayName(MakePeer(zeroWidth, "Ubuntu 24.04")) == "Ubuntu 24.04");
  UR_EXPECT_TRUE(urnw::PeerDisplayName(MakePeer(zeroWidth, zeroWidth)) == kPeerClientId);
  // the location takes the same name the row shows
  UR_EXPECT_TRUE(urnw::PeerConnectLocation<Location>(MakePeer("", "Ubuntu 24.04")).name ==
                 std::optional<std::string>("Ubuntu 24.04"));
}

UR_TEST(locationsSheetPeerRowConnectsThroughPeerConnectLocation) {
  const std::string source = ReadSource("LocationsSheet.cpp");
  UR_EXPECT_TRUE(!source.empty());
  const std::string body = DefinitionBody(source, "Gtk::Button* LocationsSheet::MakePeerRow(");
  UR_EXPECT_TRUE(!body.empty());
  UR_EXPECT_TRUE(
      body.find("host_.ConnectFromRow(PeerConnectLocation<urnet::ConnectLocation>(peerCopy));") !=
      std::string::npos);
}

UR_TEST(networkPagePeerRowsConnectThroughPeerConnectLocation) {
  const std::string source = ReadSource("NetworkPage.cpp");
  UR_EXPECT_TRUE(!source.empty());
  const std::string body = DefinitionBody(source, "void NetworkPage::Render(");
  UR_EXPECT_TRUE(!body.empty());
  UR_EXPECT_TRUE(
      body.find("host_.ConnectFromRow(PeerConnectLocation<urnet::ConnectLocation>(copy));") !=
      std::string::npos);
}

// The provider-only device's peers reach the GUI with no PeerViewController,
// so the GUI filters them as the controller filters a session's: the ones that
// provide, in the device's order.
UR_TEST(connectablePeersKeepTheProvidingOnesInOrder) {
  using List = std::vector<Peer>;
  Peer office = MakePeer("Office laptop", "Ubuntu 24.04");
  office.ClientId = std::string("a");
  Peer phone = MakePeer("Phone", "Android 16");
  phone.ClientId = std::string("b");
  phone.ProvideEnabled = false;
  Peer desk = MakePeer("Desk", "Fedora 43");
  desk.ClientId = std::string("c");
  const List connectable = urnw::ConnectablePeers(List{office, phone, desk});
  UR_EXPECT_EQ(size_t(2), connectable.size());
  if (connectable.size() != 2) return;
  UR_EXPECT_TRUE(connectable[0].ClientId == std::optional<std::string>("a"));
  UR_EXPECT_TRUE(connectable[1].ClientId == std::optional<std::string>("c"));
  UR_EXPECT_TRUE(urnw::ConnectablePeers(List{phone}).empty());
  UR_EXPECT_TRUE(urnw::ConnectablePeers(List{}).empty());
}

// Only a change in what a row shows or connects with is pushed again.
UR_TEST(samePeerRowsComparesEveryFieldARowUses) {
  using List = std::vector<Peer>;
  const Peer office = MakePeer("Office laptop", "Ubuntu 24.04");
  UR_EXPECT_TRUE(urnw::SamePeerRows(List{office}, List{office}));
  UR_EXPECT_TRUE(urnw::SamePeerRows(List{}, List{}));
  UR_EXPECT_FALSE(urnw::SamePeerRows(List{office}, List{}));
  Peer renamed = office;
  renamed.DeviceName = "Home laptop";
  UR_EXPECT_FALSE(urnw::SamePeerRows(List{office}, List{renamed}));
  Peer respecced = office;
  respecced.DeviceSpec = "Ubuntu 26.04";
  UR_EXPECT_FALSE(urnw::SamePeerRows(List{office}, List{respecced}));
  Peer stopped = office;
  stopped.ProvideEnabled = false;
  UR_EXPECT_FALSE(urnw::SamePeerRows(List{office}, List{stopped}));
  Peer other = office;
  other.ClientId = std::string("another");
  UR_EXPECT_FALSE(urnw::SamePeerRows(List{office}, List{other}));
  // what a row never shows does not count
  Peer principal = office;
  principal.Principal = "someone";
  UR_EXPECT_TRUE(urnw::SamePeerRows(List{office}, List{principal}));
  // the order is the rows' order
  Peer second = MakePeer("Desk", "Fedora 43");
  second.ClientId = std::string("c");
  UR_EXPECT_FALSE(urnw::SamePeerRows(List{office, second}, List{second, office}));
}

// a hand-built client id location is how the rows lost the flag
UR_TEST(noPeerRowBuildsItsLocationByHand) {
  for (const char* name : {"LocationsSheet.cpp", "NetworkPage.cpp"}) {
    const std::string source = ReadSource(name);
    UR_EXPECT_TRUE(!source.empty());
    if (Count(source, "client_id =") != 0)
      UR_FAIL(std::string(name) + " assigns a client id itself");
    if (Count(source, "PeerConnectLocation<urnet::ConnectLocation>(") != 1)
      UR_FAIL(std::string(name) + ": expected its one peer row to use PeerConnectLocation");
  }
}

}  // namespace
