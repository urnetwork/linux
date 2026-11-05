// The location feed (LocationFeed.hpp): the chooser says whether the provider
// list is loading, failed or found nothing, instead of reading all three as a
// blank sheet; an open view controller runs the search again when it moved or
// its last answer failed; and the filter, shared by the chooser and the
// Network page, is reapplied by each when it shows. The surfaces need GTK and
// the SDK, so the wiring cases read their source.
// SPDX-License-Identifier: MPL-2.0
#include <fstream>
#include <sstream>
#include <string>

#include "LocationFeed.hpp"
#include "TestHarness.hpp"

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

std::string ReadFeedSource(const std::string& name) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + name, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// From `signature` to the closing brace in column 0; empty when it is gone.
std::string FeedBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool FeedHas(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

bool FeedBefore(const std::string& text, const std::string& first, const std::string& second) {
  const size_t a = text.find(first);
  const size_t b = text.find(second);
  return a != std::string::npos && b != std::string::npos && a < b;
}

}  // namespace

UR_TEST(LocationFeed_TheQueryIsTrimmedAsTheSdkTrimsIt) {
  UR_EXPECT_TRUE(std::string() == urnw::TrimLocationQuery("   "));
  UR_EXPECT_TRUE(std::string("de") == urnw::TrimLocationQuery("  de\t"));
  UR_EXPECT_TRUE(std::string("new york") == urnw::TrimLocationQuery("new york "));
}

UR_TEST(LocationFeed_RowsAnswerTheQueryWhateverTheState) {
  using urnw::LocationFeedNotice;
  UR_EXPECT_TRUE(urnw::ChooserFeedNotice("", true, true, true) == LocationFeedNotice::None);
  UR_EXPECT_TRUE(urnw::ChooserFeedNotice(urnw::kLocationFeedLoading, true, true, false) ==
                 LocationFeedNotice::None);
}

UR_TEST(LocationFeed_EmptyBucketsReadByTheirState) {
  using urnw::LocationFeedNotice;
  // a loading push carries the same empty buckets as "no providers"
  UR_EXPECT_TRUE(urnw::ChooserFeedNotice(urnw::kLocationFeedLoading, true, false, true) ==
                 LocationFeedNotice::Loading);
  UR_EXPECT_TRUE(urnw::ChooserFeedNotice("LOCATIONS_LOADED", false, false, false) ==
                 LocationFeedNotice::Loading);
  UR_EXPECT_TRUE(urnw::ChooserFeedNotice("LOCATIONS_LOADED", true, false, true) ==
                 LocationFeedNotice::NoResults);
}

UR_TEST(LocationFeed_AFailedSearchIsNotAnsweredByThePreviousRows) {
  using urnw::LocationFeedNotice;
  UR_EXPECT_TRUE(urnw::ChooserFeedNotice(urnw::kLocationFeedError, true, true, true) ==
                 LocationFeedNotice::Failed);
  UR_EXPECT_TRUE(urnw::ChooserFeedNotice(urnw::kLocationFeedError, true, false, false) ==
                 LocationFeedNotice::Failed);
  // a failed idle refresh keeps a usable list
  UR_EXPECT_TRUE(urnw::ChooserFeedNotice(urnw::kLocationFeedError, true, true, false) ==
                 LocationFeedNotice::None);
}

UR_TEST(LocationFeed_TheFilterRunsWhenItMovedOrFailed) {
  UR_EXPECT_TRUE(urnw::LocationFilterRunNeeded(true, "LOCATIONS_LOADED"));
  UR_EXPECT_TRUE(urnw::LocationFilterRunNeeded(false, urnw::kLocationFeedError));
  UR_EXPECT_FALSE(urnw::LocationFilterRunNeeded(false, "LOCATIONS_LOADED"));
  UR_EXPECT_FALSE(urnw::LocationFilterRunNeeded(false, urnw::kLocationFeedLoading));
}

// Opening the chooser reapplies its own box before it renders: the filter is
// shared and a search left in it by the Network page would fill the sheet.
UR_TEST(LocationFeedWiring_TheChooserReappliesItsQueryAndSaysTheFeedState) {
  const std::string sheet = ReadFeedSource("LocationsSheet.cpp");
  UR_EXPECT_TRUE(!sheet.empty());
  const std::string open = FeedBody(sheet, "void LocationsSheet::Open() {");
  UR_EXPECT_TRUE(FeedBefore(open, "host_.FilterLocations(currentQuery_);", "Refresh();"));
  const std::string search = FeedBody(sheet, "void LocationsSheet::OnSearchChanged() {");
  UR_EXPECT_TRUE(FeedBefore(search, "host_.FilterLocations(currentQuery_);", "Refresh();"));
  const std::string rebuild = FeedBody(sheet, "void LocationsSheet::RebuildSections() {");
  UR_EXPECT_TRUE(FeedHas(rebuild, "!TrimLocationQuery(currentQuery_).empty()"));
  UR_EXPECT_TRUE(FeedHas(rebuild, "ChooserFeedNotice(host_.GetFilteredLocationState()"));
  UR_EXPECT_TRUE(FeedHas(rebuild, "\"adv_providers_load_failed\""));
  UR_EXPECT_TRUE(FeedHas(rebuild, "T_(\"loading\""));
}

// The Network page reapplies its box when it arms the device feed, and takes
// the locations pushes at all (they used to go nowhere).
UR_TEST(LocationFeedWiring_TheNetworkPageReappliesItsQueryAndHearsTheFeed) {
  const std::string page = ReadFeedSource("NetworkPage.cpp");
  const std::string reconcile = FeedBody(page, "void NetworkPage::ReconcileFeeds() {");
  UR_EXPECT_TRUE(FeedBefore(reconcile, "host_.FilterLocations(query_);",
                            "locations_ = host_.GetFilteredLocations();"));
  const std::string event = FeedBody(page, "void NetworkPage::OnLocationsEvent() {");
  UR_EXPECT_TRUE(FeedHas(event, "if (!navSelected_) return;"));
  const std::string window = ReadFeedSource("MainWindow.cpp");
  UR_EXPECT_TRUE(FeedHas(window, "event == DrawerEvent::Locations) {\n        "
                                 "networkPage_->OnLocationsEvent();"));
}

// The host keeps the wanted query across view controllers, runs it on a new
// one instead of start(), retries a failed load, and never takes mutex_ on
// the main loop while a command holds it.
UR_TEST(LocationFeedWiring_TheHostHoldsTheQueryAndRetries) {
  const std::string host = ReadFeedSource("SdkHost.cpp");
  const std::string filter = FeedBody(host, "void SdkHost::FilterLocations(");
  UR_EXPECT_TRUE(FeedHas(filter, "TrimLocationQuery(query)"));
  UR_EXPECT_TRUE(FeedBefore(filter, "if (OnMainLoop() && CommandsBusy()) {",
                            "std::scoped_lock lock(mutex_);"));
  UR_EXPECT_TRUE(FeedHas(filter, "locationsFilterOwed_.store(true);"));
  const std::string run = FeedBody(host, "void SdkHost::RunWantedLocationFilterLocked() {");
  UR_EXPECT_TRUE(FeedHas(run, "LocationFilterRunNeeded(changed, "
                              "locationsVc_->getFilteredLocationState())"));
  const std::string subscribe = FeedBody(host, "void SdkHost::SubscribeDrawer() {");
  UR_EXPECT_TRUE(FeedBefore(subscribe, "if (wanted.empty()) {\n      locationsVc_->start();",
                            "locationsVc_->filterLocations(wanted);"));
  UR_EXPECT_TRUE(FeedHas(host, "if (locationsFilterOwed_.load()) {"));
}
