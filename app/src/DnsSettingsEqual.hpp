// Field-wise equality of two traffic dns settings: the selection (PresetId and
// PresetPersonalId), the resolver switches, Fast DNS on connect and every
// list in order, a nullopt list the same as an empty one. The mask is not
// part of it: no screen edits it, and the sdk fills in its default when it is
// empty. The one comparison for "are these settings the same": the DNS
// sheet's compare-and-set and change detection (DnsSheet::SettingsEqual), and
// SdkHost's sync of its mirror with the daemon's settings.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>

#include <urnetwork_sdk.hpp>

namespace urnw {

inline bool DnsListsEqual(const std::optional<urnet::StringList>& a,
                          const std::optional<urnet::StringList>& b) {
  static const urnet::StringList kEmpty;
  const urnet::StringList& av = a ? *a : kEmpty;
  const urnet::StringList& bv = b ? *b : kEmpty;
  return av == bv;
}

inline bool DnsSettingsEqual(const urnet::DnsResolverSettings& a,
                             const urnet::DnsResolverSettings& b) {
  return a.PresetId == b.PresetId && a.PresetPersonalId == b.PresetPersonalId &&
         a.EnableRemoteDoh == b.EnableRemoteDoh && a.EnableLocalDoh == b.EnableLocalDoh &&
         a.EnableRemoteDns == b.EnableRemoteDns && a.EnableLocalDns == b.EnableLocalDns &&
         a.EnableFallback == b.EnableFallback &&
         DnsListsEqual(a.RemoteDohUrlsIpv4, b.RemoteDohUrlsIpv4) &&
         DnsListsEqual(a.RemoteDohUrlsIpv6, b.RemoteDohUrlsIpv6) &&
         DnsListsEqual(a.LocalDohUrlsIpv4, b.LocalDohUrlsIpv4) &&
         DnsListsEqual(a.LocalDohUrlsIpv6, b.LocalDohUrlsIpv6) &&
         DnsListsEqual(a.RemoteDnsIpv4, b.RemoteDnsIpv4) &&
         DnsListsEqual(a.RemoteDnsIpv6, b.RemoteDnsIpv6) &&
         DnsListsEqual(a.LocalDnsIpv4, b.LocalDnsIpv4) &&
         DnsListsEqual(a.LocalDnsIpv6, b.LocalDnsIpv6);
}

}  // namespace urnw
