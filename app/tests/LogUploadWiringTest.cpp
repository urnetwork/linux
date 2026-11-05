// The call sites of "send feedback with logs" while disconnected (support inbox
// 2090). The lifecycle is pure and tested in LogUploadTest.cpp, the wire in
// ControlProtocolTest.cpp; what makes the daemon upload its own logs with no
// tunnel, behind the log's gate, on a device that touches nothing else, off its
// main loop and one at a time — and what makes the GUI ask it first, fall back
// to its old path and learn the outcome from status — lives in the daemon and
// the GUI, which need glib, GTK and the SDK. So this reads their sources: a
// decision with no caller is this project's most-repeated defect.
//
// SPDX-License-Identifier: MPL-2.0
#include "TestHarness.hpp"

#include <fstream>
#include <sstream>
#include <string>

#ifndef UR_SRC_DIR
#define UR_SRC_DIR ""
#endif

namespace {

std::string ReadUploadSource(const std::string& relative) {
  std::ifstream in(std::string(UR_SRC_DIR) + "/" + relative, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string UploadFunctionBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) return std::string();
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// `first` occurs, and before `second` (which must occur too).
bool Ahead(const std::string& text, const std::string& first, const std::string& second) {
  const size_t a = text.find(first);
  const size_t b = text.find(second);
  return a != std::string::npos && b != std::string::npos && a < b;
}

}  // namespace

// The daemon serves the verb behind the log's gate: read-log was checked in
// Dispatch (ActionIdForVerb), and the log must be this caller's, as for
// log_tail. Sending logs owns nothing, so it claims nothing and passes no
// tunnel-owner gate a disconnected user could fail. The reply names the upload.
// Past the gate it takes the GUI's log files that came with the frame, by the
// names the request gave them, and hands them to the upload.
UR_TEST(LogUploadWiring_TheDaemonServesItBehindTheLogsGate) {
  const std::string server = ReadUploadSource("daemon/ControlServer.cpp");
  const std::string dispatch =
      UploadFunctionBody(server, "void ControlServer::DispatchAuthorized(");
  UR_EXPECT_TRUE(Ahead(dispatch, "case ctl::Verb::UploadLogs:",
                       "reply(HandleUploadLogs(conn, id, request));"));
  const std::string handle =
      UploadFunctionBody(server, "nlohmann::json ControlServer::HandleUploadLogs(");
  UR_EXPECT_TRUE(!handle.empty());
  UR_EXPECT_TRUE(Ahead(handle, "conn->peer.uid != 0 && LogBelongsToOtherUid(conn)",
                       "tunnel_.UploadLogs(req, std::move(guiLogFiles))"));
  UR_EXPECT_TRUE(Ahead(handle, "LogBelongsToOtherUid(conn)",
                       "logupload::TakePassedLogFiles(conn->passedFds, req.gui_log_files)"));
  UR_EXPECT_TRUE(Ahead(handle, "logupload::TakePassedLogFiles(conn->passedFds, req.gui_log_files)",
                       "tunnel_.UploadLogs(req, std::move(guiLogFiles))"));
  UR_EXPECT_TRUE(Ahead(handle, "LogBelongsToOtherUid(conn)", "ctl::kCodeAuthNotTunnelOwner"));
  UR_EXPECT_TRUE(Contains(handle, "request.get<ctl::UploadLogsRequest>()"));
  UR_EXPECT_TRUE(Contains(handle, "payload.carrier = result.carrier;"));
  UR_EXPECT_TRUE(Contains(handle, "payload.upload_id = result.uploadId;"));
  UR_EXPECT_TRUE(Contains(handle, "result.code"));
  UR_EXPECT_FALSE(Contains(handle, "ClaimTunnelOwnership("));
  UR_EXPECT_FALSE(Contains(handle, "CheckTunnelOwner("));
}

// UploadLogs validates before anything, admits one upload at a time (busy
// otherwise), and a bring-up that owns the session queues the admitted request
// instead of failing it; only a free session starts it.
UR_TEST(LogUploadWiring_TheDaemonValidatesThenQueuesBehindABringUp) {
  const std::string host = ReadUploadSource("daemon/TunnelHost.cpp");
  const std::string upload =
      UploadFunctionBody(host, "TunnelHost::LogUploadResult TunnelHost::UploadLogs(");
  UR_EXPECT_TRUE(!upload.empty());
  UR_EXPECT_TRUE(Ahead(upload, "ctl::ValidateUploadLogsRequest(request)", "lock.try_lock()"));
  UR_EXPECT_TRUE(Ahead(upload, "busy_.load() || !lock.try_lock()",
                       "logUploadFlight_->Begin(/*queued=*/true, logupload::Carrier::Queued, "
                       "nowMillis)"));
  UR_EXPECT_TRUE(Ahead(upload, "logUploadFlight_->Begin(/*queued=*/true", "if (uploadId == 0) {"));
  UR_EXPECT_TRUE(Ahead(upload, "if (uploadId == 0) {", "ctl::kCodeLogUploadBusy"));
  UR_EXPECT_TRUE(Ahead(upload, "ctl::kCodeLogUploadBusy", "queuedUpload_ = request;"));
  // a queued request keeps the GUI's log files that came with it
  UR_EXPECT_TRUE(Ahead(upload, "queuedUpload_ = request;",
                       "queuedGuiLogFiles_ = std::move(guiLogFiles);"));
  UR_EXPECT_TRUE(Contains(upload, "queuedUploadId_ = uploadId;"));
  UR_EXPECT_TRUE(Contains(upload, "result.uploadId = uploadId;"));
  UR_EXPECT_TRUE(Contains(upload, "logupload::Carrier::Queued"));
  UR_EXPECT_TRUE(Ahead(upload, "queuedUpload_ = request;",
                       "return StartLogUploadLocked(request, /*queuedUploadId=*/0, "
                       "std::move(guiLogFiles));"));
  UR_EXPECT_FALSE(Contains(upload, "NewDeviceLocked("));
}

// The upload is admitted before anything is built, then handed to its own
// thread with the handle of the device that runs, or of a standalone one built
// as the provider-only device is — and nothing after that. The control request
// returns as soon as the thread has it: the sdk's call (the zip) is not made
// here, and the thread takes nothing of the host.
UR_TEST(LogUploadWiring_TheUploadRunsOffTheMainLoopOnTheLiveDeviceOrAStandaloneOne) {
  const std::string host = ReadUploadSource("daemon/TunnelHost.cpp");
  const std::string start = UploadFunctionBody(
      host, "TunnelHost::LogUploadResult TunnelHost::StartLogUploadLocked(");
  UR_EXPECT_TRUE(!start.empty());
  UR_EXPECT_TRUE(
      Contains(start, "logupload::CarrierFor(device_.has_value(), providerDevice_.has_value())"));
  UR_EXPECT_TRUE(Ahead(start, "logUploadFlight_->Start(queuedUploadId, carrier, nowMillis)",
                       "NewDeviceLocked("));
  UR_EXPECT_TRUE(Ahead(start, "logUploadFlight_->Begin(/*queued=*/false, carrier, nowMillis)",
                       "NewDeviceLocked("));
  UR_EXPECT_TRUE(Ahead(start, "if (uploadId == 0) {", "NewDeviceLocked("));
  UR_EXPECT_TRUE(Contains(start, "deviceHandle = device_->handle();"));
  UR_EXPECT_TRUE(Contains(start, "deviceHandle = providerDevice_->handle();"));
  UR_EXPECT_TRUE(Ahead(start, "RetireUploadDeviceLocked();", "uploadDevice_ = NewDeviceLocked("));
  UR_EXPECT_TRUE(Ahead(start, "LoadNetworkSpaceLocked(request.network_space_json);",
                       "uploadDevice_ = NewDeviceLocked("));
  UR_EXPECT_TRUE(Contains(start,
                          "uploadDevice_ = NewDeviceLocked(request.by_jwt, request.instance_id, "
                          "request.app_version);"));
  UR_EXPECT_TRUE(Ahead(start, "uploadDevice_ = NewDeviceLocked(",
                       "uploadDevice_->setProvideControlMode(\"never\");"));
  UR_EXPECT_TRUE(Contains(start, "deviceHandle = uploadDevice_->handle();"));
  // call forms, so a comment that names them does not count
  for (const char* forbidden :
       {"Tunnel::Open(", "egressMarker_.", "ApplyFilterLocked(", "InstallFilterLocked(",
        "setRpcServer(", "newIoLoop(", "setTunnelStarted(", " device_ = ", "providerDevice_ = ",
        "ClaimTunnelOwnership(", "OpenProviderViewControllersLocked(",
        // the sdk's call (the zip) and the line before it belong to the thread
        "->uploadLogs(", "urnet_device_upload_logs(", "urnet_device_local_upload_logs_with_files(",
        "urnet::logAppInfo("}) {
    UR_EXPECT_TRUE_MSG(forbidden, !Contains(start, forbidden));
  }
  UR_EXPECT_TRUE(Ahead(start, "logUploadFlight_->Run(uploadId, deviceHandle,", "result.ok = true;"));
  const size_t runAt = start.find("logUploadFlight_->Run(");
  const size_t runEnd = start.find("});", runAt == std::string::npos ? 0 : runAt);
  const std::string run = runAt == std::string::npos || runEnd == std::string::npos
                              ? std::string()
                              : start.substr(runAt, runEnd - runAt);
  UR_EXPECT_TRUE(Contains(run, "[flight = logUploadFlight_, uploadId, deviceHandle,"));
  UR_EXPECT_TRUE(Contains(run, "UploadLogsOnDevice(flight, uploadId, deviceHandle, feedbackId,"));
  // the GUI's log files go with the call, owned by the thread from here
  UR_EXPECT_TRUE(Contains(run, "guiLogFiles = std::make_shared<logupload::PassedLogFiles>("));
  UR_EXPECT_TRUE(Contains(run, "carrierName, *guiLogFiles);"));
  UR_EXPECT_FALSE(Contains(run, "this"));
  UR_EXPECT_TRUE(Contains(start, "result.uploadId = uploadId;"));
  UR_EXPECT_TRUE(Ahead(start, "catch (const std::exception& e)",
                       "logUploadFlight_->Finish(uploadId, logupload::FlightState::Failed);"));
  UR_EXPECT_TRUE(Contains(start, "ctl::kCodeLogUploadFailed"));
}

// The upload's thread writes the line naming the carrier into the files the
// zip takes, then calls the sdk by the device's handle; its callback ends the
// upload in the flight with the server's answer. Neither touches the host.
UR_TEST(LogUploadWiring_TheUploadsThreadAndCallbackTouchOnlyTheFlight) {
  const std::string host = ReadUploadSource("daemon/TunnelHost.cpp");
  const std::string thread = UploadFunctionBody(host, "void UploadLogsOnDevice(");
  UR_EXPECT_TRUE(!thread.empty());
  UR_EXPECT_TRUE(Ahead(thread, "urnet::logAppInfo(\"log-upload\"",
                       "urnet_device_local_upload_logs_with_files("));
  UR_EXPECT_TRUE(Contains(thread, "&OnLogUploadReport,\n      report.get(), &error);"));
  UR_EXPECT_TRUE(Ahead(thread, "if (started) {", "report.release();"));
  UR_EXPECT_TRUE(Contains(thread, "flight->Finish(uploadId, logupload::FlightState::Failed);"));
  UR_EXPECT_TRUE(Contains(thread, "urnet_free_string(error);"));
  const std::string callback = UploadFunctionBody(host, "void OnLogUploadReport(");
  UR_EXPECT_TRUE(!callback.empty());
  UR_EXPECT_TRUE(Contains(callback, "logupload::FlightState::Refused"));
  UR_EXPECT_TRUE(Contains(callback, "logupload::FlightState::Failed"));
  UR_EXPECT_TRUE(Contains(callback, "report->flight->Finish(report->uploadId, state);"));
  for (const std::string& body : {thread, callback}) {
    UR_EXPECT_FALSE(Contains(body, "TunnelHost"));
    UR_EXPECT_FALSE(Contains(body, "opMutex_"));
  }
}

// The standalone device never shares a moment with another device under the
// same identity: every teardown (and so the head of every bring-up) and every
// provider-only start retire it first, the provider's only after its refusals.
UR_TEST(LogUploadWiring_EveryOtherDeviceRetiresTheStandaloneOneFirst) {
  const std::string host = ReadUploadSource("daemon/TunnelHost.cpp");
  const std::string stop =
      UploadFunctionBody(host, "void TunnelHost::StopInternalLocked(const std::string& reason, bool disconnect)");
  UR_EXPECT_TRUE(Ahead(stop, "RetireUploadDeviceLocked();", "ReleaseDeviceLocked(device_);"));
  const std::string provider = UploadFunctionBody(
      host, "TunnelHost::ProviderStartResult TunnelHost::StartProvider(");
  UR_EXPECT_TRUE(Ahead(provider, "ctl::kCodeKillSwitchArmed", "RetireUploadDeviceLocked();"));
  UR_EXPECT_TRUE(
      Ahead(provider, "RetireUploadDeviceLocked();", "providerDevice_ = NewDeviceLocked("));
  const std::string retire =
      UploadFunctionBody(host, "void TunnelHost::RetireUploadDeviceLocked()");
  UR_EXPECT_TRUE(Ahead(retire, "uploadDevice_->close();", "ReleaseDeviceLocked(uploadDevice_);"));
  UR_EXPECT_TRUE(Contains(retire, "uploadDevice_->waitForClose("));
}

// The device the upload's call is on stays alive until that call returns:
// each of the three releases goes through the flight, which keeps it then,
// after the device's usual close.
UR_TEST(LogUploadWiring_TheCallsDeviceStaysAliveUntilTheCallReturns) {
  const std::string host = ReadUploadSource("daemon/TunnelHost.cpp");
  const std::string release =
      UploadFunctionBody(host, "void TunnelHost::ReleaseDeviceLocked(");
  UR_EXPECT_TRUE(Contains(release,
                          "logUploadFlight_->KeepUntilReturned(\n      deviceHandle, "
                          "std::make_shared<urnet::DeviceLocal>(std::move(*device)));"));
  UR_EXPECT_TRUE(Ahead(release, "KeepUntilReturned(", "device.reset();"));
  const std::string stop =
      UploadFunctionBody(host, "void TunnelHost::StopInternalLocked(const std::string& reason, bool disconnect)");
  UR_EXPECT_TRUE(Ahead(stop, "device_->close();", "ReleaseDeviceLocked(device_);"));
  UR_EXPECT_FALSE(Contains(stop, "device_.reset();"));
  const std::string retireProvider =
      UploadFunctionBody(host, "void TunnelHost::RetireProviderDeviceLocked()");
  UR_EXPECT_TRUE(Ahead(retireProvider, "providerDevice_->close();",
                       "ReleaseDeviceLocked(providerDevice_);"));
  UR_EXPECT_FALSE(Contains(retireProvider, "providerDevice_.reset();"));
  // the daemon's teardown gives a zipping upload a moment, and no more
  const std::string teardown = UploadFunctionBody(host, "TunnelHost::~TunnelHost()");
  UR_EXPECT_TRUE(Ahead(teardown, "Stop(\"daemon_shutdown\");",
                       "logUploadFlight_->WaitReturned(kLogUploadReturnBudget)"));
}

// The reaper retires the standalone device once its upload finished (or at
// the bound), and starts a queued request once no bring-up owns the session;
// one queued too long is dropped, and its outcome says so.
UR_TEST(LogUploadWiring_TheReaperRetiresAndStartsQueuedUploads) {
  const std::string host = ReadUploadSource("daemon/TunnelHost.cpp");
  const std::string reap = UploadFunctionBody(host, "void TunnelHost::Reap()");
  UR_EXPECT_TRUE(Ahead(reap, "if (busy_.load()) return;", "MaintainLogUploadLocked();"));
  UR_EXPECT_TRUE(Ahead(reap, "if (!lock.owns_lock()) return;", "MaintainLogUploadLocked();"));
  const std::string maintain =
      UploadFunctionBody(host, "void TunnelHost::MaintainLogUploadLocked()");
  UR_EXPECT_TRUE(Contains(maintain, "logUploadFlight_->Read(nowMillis)"));
  UR_EXPECT_TRUE(Contains(maintain, "logupload::RetireStandaloneDevice("));
  UR_EXPECT_TRUE(Contains(maintain, "logupload::IsFinished(upload.state)"));
  UR_EXPECT_TRUE(
      Ahead(maintain, "logupload::RetireStandaloneDevice(", "RetireUploadDeviceLocked();"));
  UR_EXPECT_TRUE(Ahead(maintain, "logupload::QueuedUploadExpired(",
                       "StartLogUploadLocked(request, uploadId, std::move(guiLogFiles));"));
  UR_EXPECT_TRUE(Ahead(maintain,
                       "logupload::PassedLogFiles guiLogFiles = std::move(queuedGuiLogFiles_);",
                       "logupload::QueuedUploadExpired("));
  UR_EXPECT_TRUE(Ahead(maintain, "logupload::QueuedUploadExpired(",
                       "logUploadFlight_->Finish(uploadId, logupload::FlightState::Failed);"));
}

// Status carries the upload, read off the flight's own lock before the status
// lock: how the outcome reaches the GUI.
UR_TEST(LogUploadWiring_TheStatusCarriesTheUpload) {
  const std::string host = ReadUploadSource("daemon/TunnelHost.cpp");
  const std::string status = UploadFunctionBody(host, "ctl::StatusReply TunnelHost::Status() const");
  UR_EXPECT_TRUE(Ahead(status, "logUploadFlight_->Read(MonotonicMillis())",
                       "std::scoped_lock lock(statusMutex_);"));
  for (const char* field : {"s.log_upload_id = upload.id;",
                            "s.log_upload_state = logupload::ToString(upload.state);",
                            "s.log_upload_carrier ="}) {
    UR_EXPECT_TRUE_MSG(field, Contains(status, field));
  }
}

// The GUI asks the daemon first, with the credentials start_provider carries,
// without holding its own lock across the call, and falls back to the
// DeviceRemote only when the daemon did not take it (not when it is busy).
UR_TEST(LogUploadWiring_TheGuiAsksTheDaemonFirstAndFallsBack) {
  const std::string page = ReadUploadSource("SupportPage.cpp");
  const std::string upload =
      UploadFunctionBody(page, "void SupportPage::UploadLogs(const std::string& feedbackId)");
  UR_EXPECT_TRUE(!upload.empty());
  UR_EXPECT_TRUE(Ahead(upload, "feedbackId.empty()", "host_.UploadDaemonLogs(feedbackId)"));
  UR_EXPECT_TRUE(Contains(upload, "logupload::GuiStepAfterDaemon(answer, host_.hasDevice())"));
  UR_EXPECT_TRUE(Ahead(upload, "host_.UploadDaemonLogs(feedbackId)", "host_.device().uploadLogs("));
  UR_EXPECT_TRUE(Ahead(upload, "logupload::GuiStep::Done", "host_.device().uploadLogs("));
  // still only after the server accepted the feedback, with the box ticked
  const std::string send = UploadFunctionBody(page, "void SupportPage::OnSendFeedback()");
  UR_EXPECT_TRUE(Contains(send, "if (attachLogs && !feedbackId.empty()) UploadLogs(feedbackId);"));

  const std::string sdk = ReadUploadSource("SdkHost.cpp");
  const std::string daemon = UploadFunctionBody(
      sdk, "logupload::DaemonAnswer SdkHost::UploadDaemonLogs(const std::string& feedbackId)");
  UR_EXPECT_TRUE(!daemon.empty());
  for (const char* field :
       {"request.feedback_id = feedbackId;", "request.by_jwt = localState_->getByClientJwt();",
        "request.instance_id = localState_->getInstanceId();",
        "request.app_version = kAppVersion;",
        "request.network_space_json = networkSpace_->toJson();"}) {
    UR_EXPECT_TRUE_MSG(field, Contains(daemon, field));
  }
  // the lock is scoped to reading the session, and the call comes after it
  UR_EXPECT_TRUE(Contains(daemon, "{\n    std::scoped_lock lock(mutex_);"));
  UR_EXPECT_TRUE(Ahead(daemon, "request.network_space_json = networkSpace_->toJson();\n",
                       "control_.UploadLogs(request, guiLogFiles, &carrier, &error, &code, "
                       "&uploadId)"));
  const size_t lockAt = daemon.find("std::scoped_lock lock(mutex_);");
  const size_t scopeEnd = daemon.find("\n  }\n", lockAt == std::string::npos ? 0 : lockAt);
  const size_t callAt = daemon.find("control_.UploadLogs(");
  UR_EXPECT_TRUE(lockAt != std::string::npos && scopeEnd != std::string::npos &&
                 callAt != std::string::npos && scopeEnd < callAt);
  // and nothing takes it again before the call returns
  UR_EXPECT_TRUE(scopeEnd != std::string::npos &&
                 daemon.find("mutex_", scopeEnd) == std::string::npos);
  // the upload it waits on, and an answer that one is in flight already
  UR_EXPECT_TRUE(Ahead(daemon,
                       "control_.UploadLogs(request, guiLogFiles, &carrier, &error, &code, "
                       "&uploadId)",
                       "pendingLogUploadId_ = uploadId;"));
  UR_EXPECT_TRUE(Ahead(daemon, "code == ctl::kCodeLogUploadBusy",
                       "return logupload::DaemonAnswer::Busy;"));
}

// The outcome reaches the GUI: the health poll follows the daemon's status in
// both branches, with the one status its worker read, and the upload the
// reply named ends the wait once status names it finished.
UR_TEST(LogUploadWiring_TheGuiLearnsTheOutcomeFromStatus) {
  const std::string window = ReadUploadSource("MainWindow.cpp");
  const std::string poll = UploadFunctionBody(
      window, "void MainWindow::ApplyDaemonHealth(const std::optional<ctl::StatusReply>& status)");
  const size_t idle = poll.find("if (!connected_) {");
  const size_t connected = poll.find("if (!status) return;");
  UR_EXPECT_TRUE(idle != std::string::npos && connected != std::string::npos);
  UR_EXPECT_TRUE(Ahead(poll, "if (!connected_) {", "host_.FollowDaemonLogUpload(*status);"));
  UR_EXPECT_TRUE(Ahead(poll.substr(connected == std::string::npos ? 0 : connected),
                       "if (!status) return;", "host_.FollowDaemonLogUpload(*status);"));

  const std::string sdk = ReadUploadSource("SdkHost.cpp");
  const std::string follow =
      UploadFunctionBody(sdk, "void SdkHost::FollowDaemonLogUpload(const ctl::StatusReply& status)");
  UR_EXPECT_TRUE(Ahead(follow, "if (status.redacted) return;", "logupload::CompletionFor("));
  UR_EXPECT_TRUE(Contains(follow, "logupload::CompletionFor(\n      pendingLogUploadId_, "
                                  "status.log_upload_id,"));
  UR_EXPECT_TRUE(Ahead(follow, "if (!outcome) return;", "pendingLogUploadId_ = 0;"));
}

// The client validates before a frame is sent and does not re-send a frame the
// daemon received: a second one would start a second upload.
UR_TEST(LogUploadWiring_TheClientValidatesAndDoesNotResend) {
  const std::string client = ReadUploadSource("ControlClient.cpp");
  const std::string upload = UploadFunctionBody(client, "bool ControlClient::UploadLogs(");
  UR_EXPECT_TRUE(!upload.empty());
  UR_EXPECT_TRUE(Ahead(upload, "ctl::ValidateUploadLogsRequest(request)",
                       "CallLocked(ctl::Verb::UploadLogs, nlohmann::json(withGuiLogFiles), error,"));
  UR_EXPECT_TRUE(Contains(upload, "/*allowRetry=*/false"));
  UR_EXPECT_TRUE(Contains(upload, "reply->get<ctl::UploadLogsReply>()"));
  UR_EXPECT_TRUE(Contains(upload, "if (uploadId) *uploadId = payload.upload_id;"));
}

// The GUI's own log files ride in the daemon's zip (PassedLogFiles.hpp): the
// GUI flushes its glog, takes the newest files its own upload would send (the
// sdk's inventory), opens each itself without following a link, keeps only
// regular files, and hands the descriptors over with the request's frame. The
// daemon never opens a path the GUI names.
UR_TEST(LogUploadWiring_TheGuiHandsOverItsOwnLogFilesByDescriptor) {
  const std::string sdk = ReadUploadSource("SdkHost.cpp");
  const std::string open = UploadFunctionBody(sdk, "logupload::PassedLogFiles OpenGuiLogFiles()");
  UR_EXPECT_TRUE(!open.empty());
  UR_EXPECT_TRUE(Ahead(open, "urnet::flushGlog();", "urnet::uploadLogsInventory()"));
  UR_EXPECT_TRUE(Contains(open, "files.Size() >= logupload::kMaxGuiLogFiles"));
  UR_EXPECT_TRUE(Contains(open, "logupload::LooksLikeGlogFileName(info.Name)"));
  UR_EXPECT_TRUE(Contains(open, "O_RDONLY | O_CLOEXEC | O_NOFOLLOW"));
  UR_EXPECT_TRUE(Ahead(open, "S_ISREG(fileStat.st_mode)", "files.Add(info.Name, fd);"));
  const std::string daemon = UploadFunctionBody(
      sdk, "logupload::DaemonAnswer SdkHost::UploadDaemonLogs(const std::string& feedbackId)");
  UR_EXPECT_TRUE(Ahead(daemon, "const logupload::PassedLogFiles guiLogFiles = OpenGuiLogFiles();",
                       "control_.UploadLogs(request, guiLogFiles,"));

  const std::string client = ReadUploadSource("ControlClient.cpp");
  const std::string upload = UploadFunctionBody(client, "bool ControlClient::UploadLogs(");
  UR_EXPECT_TRUE(Ahead(upload, "withGuiLogFiles.gui_log_files = guiLogFiles.Names();",
                       "CallLocked(ctl::Verb::UploadLogs, nlohmann::json(withGuiLogFiles), error,"));
  UR_EXPECT_TRUE(Contains(upload, "const std::vector<int> guiLogFds = guiLogFiles.Fds();"));
  UR_EXPECT_TRUE(Contains(upload, "/*receiveTimeoutSeconds=*/0, &guiLogFds);"));
  const std::string call =
      UploadFunctionBody(client, "std::optional<nlohmann::json> ControlClient::CallLocked(");
  UR_EXPECT_TRUE(Contains(call, "&frameDelivered,\n                                     passFds)"));
  const std::string roundTrip =
      UploadFunctionBody(client, "std::optional<nlohmann::json> ControlClient::RoundTripLocked(");
  UR_EXPECT_TRUE(Contains(roundTrip, "SendAllLocked(frame, &sent, passFds);"));
  const std::string send = UploadFunctionBody(client, "bool ControlClient::SendAllLocked(");
  UR_EXPECT_TRUE(
      Contains(send, "fdpass::SendWithFds(fd_, data.data() + sent, data.size() - sent,"));
  UR_EXPECT_TRUE(Contains(send, "sent == 0 ? *fds : noFds"));
  UR_EXPECT_FALSE(Contains(send, "::send("));
}

// The daemon receives descriptors only with a frame's bytes, holds no more
// than one request's worth, closes what no request claimed once no frame is
// pending, and closes the rest with the connection.
UR_TEST(LogUploadWiring_TheDaemonHoldsPassedDescriptorsOnlyForTheirRequest) {
  const std::string server = ReadUploadSource("daemon/ControlServer.cpp");
  const std::string read =
      UploadFunctionBody(server, "bool ControlServer::ReadIntoBuffer(Connection* conn)");
  UR_EXPECT_TRUE(
      Contains(read, "fdpass::ReceiveWithFds(conn->fd, buf, sizeof(buf), &fds, &truncated)"));
  UR_EXPECT_FALSE(Contains(read, "::recv("));
  UR_EXPECT_TRUE(Ahead(read, "conn->passedFds.insert(",
                       "if (conn->passedFds.size() > fdpass::kMaxFds) return false;"));
  const std::string pump =
      UploadFunctionBody(server, "bool ControlServer::PumpConnection(uint64_t connId)");
  UR_EXPECT_TRUE(Ahead(pump, "if (conn->authPending) return true;",
                       "if (conn->inBuf.empty()) ClosePassedFds(conn->passedFds);"));
  const std::string close =
      UploadFunctionBody(server, "void ControlServer::CloseConnection(Connection* conn)");
  UR_EXPECT_TRUE(Ahead(close, "ClosePassedFds(conn->passedFds);", "::close(fd);"));
}

// The upload's thread hands the GUI's files to the sdk under the GUI's folder,
// by descriptor, and closes them once the call returned: the sdk read them in
// it. A queued request dropped by a sign-out closes them too.
UR_TEST(LogUploadWiring_TheUploadCarriesTheGuisFilesAndClosesThem) {
  const std::string host = ReadUploadSource("daemon/TunnelHost.cpp");
  const std::string thread = UploadFunctionBody(host, "void UploadLogsOnDevice(");
  UR_EXPECT_TRUE(Contains(thread, "uploadLogsFile.Source = logupload::kGuiLogFilesSource;"));
  UR_EXPECT_TRUE(Contains(thread, "uploadLogsFile.FileDescriptor = file.fd;"));
  UR_EXPECT_TRUE(Ahead(thread, "ctl::DumpForWire(nlohmann::json(uploadLogsFiles))",
                       "urnet_device_local_upload_logs_with_files("));
  UR_EXPECT_TRUE(
      Contains(thread, "deviceHandle, feedbackId.c_str(), uploadLogsFilesJson.c_str(),"));
  UR_EXPECT_TRUE(
      Ahead(thread, "urnet_device_local_upload_logs_with_files(", "guiLogFiles.CloseAll();"));
  UR_EXPECT_TRUE(Ahead(thread, "guiLogFiles.CloseAll();", "if (started) {"));
  const std::string logout = UploadFunctionBody(host, "bool TunnelHost::Logout(");
  UR_EXPECT_TRUE(Ahead(logout, "queuedUpload_.reset();", "queuedGuiLogFiles_.CloseAll();"));
}
