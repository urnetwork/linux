// Compiled with actual SdkHost methods extracted by AuthSessionRegression.py.
// SDK, disk, daemon and GTK boundaries are controllable; no service is contacted.
// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "ConnectionRegion.hpp"

namespace region = urnw::region;

using Task = std::function<void()>;
std::deque<Task> mainTasks, workerTasks, storageTasks;
void PostToMain(Task task) { mainTasks.push_back(std::move(task)); }
void Step(std::deque<Task>& tasks) {
  if (tasks.empty()) throw std::runtime_error("missing scheduled task");
  auto task = std::move(tasks.front()); tasks.pop_front(); task();
}
void Main() { while (!mainTasks.empty()) Step(mainTasks); }
void Drain() { while (!mainTasks.empty() || !workerTasks.empty()) { Main(); if (!workerTasks.empty()) Step(workerTasks); } }
void AsyncStorage() { while (!storageTasks.empty()) Step(storageTasks); }
void g_message(const char*, ...) {}
void g_warning(const char*, ...) {}
long g_get_monotonic_time() { return 0; }
std::string UrDeviceDescription() { return "test"; }
std::string UrDeviceSpec() { return "test"; }

namespace urnet {
inline const std::string TAO = "tao";
inline constexpr int BittensorWalletStateSigned = 1;
struct Error { std::string message; std::optional<std::string> code; };
struct Network { std::string by_jwt; };
struct OptionalNetwork { std::optional<std::string> by_jwt; };
struct Verification { std::optional<std::string> send_error; };
struct WalletAuthArgs { std::optional<std::string> wallet_address, wallet_signature, wallet_message, blockchain; };
struct AuthLoginArgs { std::string user_auth, seedphrase, auth_jwt_type, auth_jwt; std::optional<WalletAuthArgs> wallet_auth; bool result_errors = false; };
struct AuthLoginResult { std::optional<Error> error; std::optional<Network> network; std::optional<std::string> user_auth; std::optional<std::vector<std::string>> auth_allowed; };
struct AuthLoginWithPasswordArgs { std::string user_auth, password; bool verify_otp_numeric = false; };
struct AuthLoginWithPasswordResult { std::optional<Error> error; std::optional<OptionalNetwork> network; std::optional<Verification> verification_required; };
struct AuthCodeLoginArgs { std::string auth_code; };
struct AuthCodeLoginResult { std::optional<Error> error; std::string by_jwt; };
struct AuthVerifyArgs { std::string user_auth, verify_code; };
struct AuthVerifyResult { std::optional<Error> error; std::optional<Network> network; };
struct NetworkCreateArgs { std::string user_name, user_auth, password, network_name, referral_code, auth_jwt_type, auth_jwt; bool terms = false, verify_use_numeric = false; };
struct NetworkCreateResult { std::optional<Error> error; std::optional<OptionalNetwork> network; std::optional<Verification> verification_required; std::optional<std::string> seedphrase; };
struct AuthNetworkClientArgs { std::string description, device_spec; };
struct AuthNetworkClientResult { std::optional<Error> error; std::optional<std::string> by_client_jwt; };
template<class T> using Callback = std::function<void(std::optional<T>, std::optional<std::string>)>;
using AuthNetworkClientCallback = Callback<AuthNetworkClientResult>;
}

struct Api {
  std::string jwt;
  std::vector<std::string> writes;
  std::vector<urnet::Callback<urnet::AuthLoginResult>> login;
  std::vector<urnet::Callback<urnet::AuthLoginWithPasswordResult>> password;
  std::vector<urnet::Callback<urnet::AuthCodeLoginResult>> code;
  std::vector<urnet::Callback<urnet::AuthVerifyResult>> verify;
  std::vector<urnet::Callback<urnet::NetworkCreateResult>> create;
  void setByJwt(const std::string& value) { jwt = value; writes.push_back(value); }
  void authLogin(urnet::AuthLoginArgs, urnet::Callback<urnet::AuthLoginResult> cb) { login.push_back(cb); }
  void authLoginWithPassword(urnet::AuthLoginWithPasswordArgs, urnet::Callback<urnet::AuthLoginWithPasswordResult> cb) { password.push_back(cb); }
  void authCodeLogin(urnet::AuthCodeLoginArgs, urnet::Callback<urnet::AuthCodeLoginResult> cb) { code.push_back(cb); }
  void authVerify(urnet::AuthVerifyArgs, urnet::Callback<urnet::AuthVerifyResult> cb) { verify.push_back(cb); }
  void networkCreate(urnet::NetworkCreateArgs, urnet::Callback<urnet::NetworkCreateResult> cb) { create.push_back(cb); }
};
struct LocalState {
  std::string jwt, client;
  Task onJwt, onClient, onLogout;
  bool failJwt = false, failClient = false;
  int clears = 0;
  void setByJwt(const std::string& value) { if (onJwt) std::exchange(onJwt, {})(); if (failJwt) throw std::runtime_error("disk"); jwt = value; }
  void setByClientJwt(const std::string& value) { if (onClient) std::exchange(onClient, {})(); if (failClient) throw std::runtime_error("disk"); client = value; }
  void logout() { if (onLogout) std::exchange(onLogout, {})(); jwt.clear(); client.clear(); ++clears; }
};
struct AsyncLocalState {
  std::shared_ptr<LocalState> local;
  void logout(std::function<void(bool)> done) { storageTasks.push_back([local=local,done] { local->logout(); done(true); }); }
};
struct Tracker { bool in = false; void SignedIn() { in=true; } void SignedOut() { in=false; } };
struct Flow { uint64_t value=0; uint64_t Begin() { return ++value; } uint64_t Latest() const { return value; } bool IsCurrent(uint64_t f) const { return value==f; } };
struct WalletSession { int state() { return urnet::BittensorWalletStateSigned; } void cancel() {} };
struct Wallet { std::shared_ptr<WalletSession> bittensorSession() { return {}; } };
struct CryptoInvoiceHost { int signOuts=0; void SignOut() { ++signOuts; } };
struct Control { void StopTunnel() {} };
struct Events { void NewSession() {} };
namespace signout { enum class Delivery { Delivered }; inline const char* ToString(Delivery) { return "done"; } }
struct Obligation { signout::Delivery Begin(int) { return signout::Delivery::Delivered; } };
struct Backoff { void NoteSuccess() {} void NoteFailure(long) {} };
struct ConnectCommandQueue { enum class Kind { Connect, Disconnect, Device }; bool signingOut=false; void BeginSignOut() { signingOut=true; } void EndSignOut() { signingOut=false; } bool SigningOut() const { return signingOut; } };
enum class DrawerEvent { DeviceLifecycle };
struct AuthResult { bool ok=false, verification_required=false; std::string error, authAllowed; bool sso=false, sso_needs_network=false, wallet_needs_network=false; };
struct InstantAccount { bool ok=false; std::string seedphrase, error; };
enum class LoginRoute { Error, Login, Password, IncorrectAuth, Create };
struct LoginRouting { LoginRoute route=LoginRoute::Error; std::string userAuth,error,authAllowed; };
struct WalletSignature { std::string error; };
AuthResult VerificationRequired(std::optional<std::string>) { return {false,true,""}; }

struct SdkHost {
  std::atomic<uint64_t> signInEpoch_{0};
  std::mutex mutex_, authStateMutex_;
  bool authAttemptSinceLogout_=false;
  std::atomic<bool> signedOut_{false},hasClientJwt_{true};
  Tracker authLogouts_;
  ConnectCommandQueue commands_;
  std::shared_ptr<Api> api_=std::make_shared<Api>();
  std::shared_ptr<LocalState> localState_=std::make_shared<LocalState>();
  std::shared_ptr<AsyncLocalState> asyncLocalState_=std::make_shared<AsyncLocalState>(AsyncLocalState{localState_});
  std::optional<int> device_;
  Control control_;
  std::shared_ptr<Events> events_=std::make_shared<Events>();
  std::function<void(bool,uint64_t)> onAuth_;
  std::function<void(const std::string&)> onApiUnreachable_;
  std::vector<urnet::AuthNetworkClientCallback> registrations;
  std::vector<std::string> registrationTokens;
  Wallet wallet_;
  std::unique_ptr<CryptoInvoiceHost> cryptoInvoice_=std::make_unique<CryptoInvoiceHost>();
  Flow walletFlows_;
  std::function<void(AuthResult)> walletAuthDone_,walletCreateDone_;
  std::function<void(WalletSignature)> walletSignDone_;
  uint64_t walletAuthEpoch_=0,pendingWalletAuthEpoch_=0,pendingSsoAuthEpoch_=0,pendingInstantAuthEpoch_=0;
  std::optional<urnet::WalletAuthArgs> pendingWalletAuth_;
  bool pendingSsoAuth_=false;
  std::string pendingSsoType_,pendingSsoJwt_;
  std::optional<std::string> pendingInstantJwt_;
  Backoff signOutBackoff_,providerBackoff_;
  Obligation signOut_;
  std::atomic<bool> daemonProviderRunning_{false},daemonProviderNetworkKey_{false};
  std::atomic<int> daemonProviderMode_{0},daemonProviderClientCount_{0};
  bool providerStateKnown_=false;
  bool AuthEpochCurrent(uint64_t epoch) const { return signInEpoch_.load()==epoch; }
  void SetAuthStateHandler(std::function<void(bool,uint64_t)> cb) { onAuth_=std::move(cb); }
  void ApplySignupPreferences(urnet::NetworkCreateArgs&) {}
  void CancelRowConnect(const char*) {}
  void CancelPendingAddSignIn(const char*) {}
  void TeardownDeviceLocked() { device_.reset(); }
  void EmitDrawerEvent(DrawerEvent) {}
  void ForgetRpcSession() {}
  void DropDaemonProviderStatsLocked() {}
  int SignOutDaemonLocked() { return 0; }
  bool RunCommand(ConnectCommandQueue::Kind, Task work, Task apply={}) {
    workerTasks.push_back([work,apply] { work(); PostToMain([apply] { if (apply) apply(); }); }); return true;
  }
  void AuthNetworkClientWithLocale(const urnet::AuthNetworkClientArgs&,urnet::AuthNetworkClientCallback cb) {
    registrations.push_back(cb); registrationTokens.push_back(api_->jwt);
  }
  uint64_t CaptureAuthEpoch();
  void NoteApiError(const std::optional<std::string>&);
  static bool OnMainLoop() { return true; }
  bool NetworkServerChangeAllowed();
  void StartLogin(const std::string&,std::function<void(LoginRouting)>);
  void LoginWithPassword(const std::string&,const std::string&,std::function<void(AuthResult)>);
  void LoginWithCode(const std::string&,std::function<void(AuthResult)>);
  void LoginWithSeedphrase(const std::string&,std::function<void(AuthResult)>);
  void VerifyCode(const std::string&,const std::string&,std::function<void(AuthResult)>);
  void CreateNetwork(const std::string&,const std::string&,const std::string&,const std::string&,std::function<void(AuthResult)>);
  void CreateInstantAccount(const std::string&,std::function<void(InstantAccount)>);
  void ConfirmInstantAccount(std::function<void(AuthResult)>);
  void HandleNetworkCreateResult(std::optional<urnet::NetworkCreateResult>,std::optional<std::string>,std::function<void(AuthResult)>,uint64_t);
  void AuthLoginWithSso(const std::string&,const std::string&);
  void AuthLoginWithWallet(const std::string&,const std::string&,const std::string&,const std::string&);
  void RegisterNetworkClient(const std::string&,std::function<void(AuthResult)>,uint64_t);
  void FinishNetworkClientRegistration(const std::string&,std::function<void(AuthResult)>,uint64_t);
  void Logout();
  void LogoutNow();
};
struct MainWindow {
  SdkHost& host_;
  std::vector<bool> shown;
  void ApplyAuthState(bool in) { shown.push_back(in); }
  void InstallAuthHandler();
};

// @ACTUAL_METHODS@

int checks=0;
void Check(bool ok,const std::string& why) { ++checks; if (!ok) throw std::runtime_error(why); }
void Fresh() { mainTasks.clear(); workerTasks.clear(); storageTasks.clear(); }
void SignIn(SdkHost& host,const std::string& name) {
  bool done=false;
  host.RegisterNetworkClient(name,[&](AuthResult r) { Check(r.ok,"new account must sign in"); done=true; },host.CaptureAuthEpoch());
  Drain();
  Check(!host.registrations.empty(),"registration request exists");
  host.registrations.back()(urnet::AuthNetworkClientResult{std::nullopt,name+"-client"},std::nullopt);
  Drain(); Check(done,"sign-in completes");
}
void ExpectB(SdkHost& h,const std::string& why) {
  Check(h.api_->jwt=="B" && h.localState_->jwt=="B" && h.localState_->client=="B-client",why+": credentials");
  Check(h.hasClientJwt_ && !h.signedOut_,why+": flags");
}
template<class Result> void Respond(const urnet::Callback<Result>& callback,Result success,int outcome) {
  if(outcome==0) callback(success,std::nullopt);
  if(outcome==1) callback(std::nullopt,std::string("dial tcp: connection refused"));
  if(outcome==2) callback(std::nullopt,std::nullopt);
  if(outcome==3) { success.error=urnet::Error{"old API failure",std::nullopt}; callback(success,std::nullopt); }
}

int main() {
  try {
    {
      Fresh(); SdkHost h; int unreachable=0; bool failed=false;
      h.onApiUnreachable_=[&](const std::string&) { ++unreachable; };
      h.LoginWithCode("current",[&](AuthResult r) { failed=!r.ok; });
      h.api_->code.back()(std::nullopt,std::string("dial tcp: connection refused"));
      Check(failed && unreachable==1,"current transport failure reports the connection-region prompt");
      h.NoteApiError(std::nullopt);
      h.NoteApiError(std::string("401 Unauthorized: sign-in refused"));
      Check(unreachable==1,"successful calls and API refusals do not report an unreachable service");
    }
    for (int kind=0;kind<6;++kind) for (int outcome=0;outcome<4;++outcome) {
      Fresh(); SdkHost h; bool cancelled=false; int unreachable=0;
      h.onApiUnreachable_=[&](const std::string&) { ++unreachable; };
      auto done=[&](AuthResult r) { cancelled=!r.ok && r.error=="superseded by signing out"; };
      if(kind==0) h.LoginWithPassword("user","password",done);
      if(kind==1) h.LoginWithCode("code",done);
      if(kind==2) h.LoginWithSeedphrase("a phrase",done);
      if(kind==3) h.VerifyCode("user","code",done);
      if(kind==4) h.StartLogin("user",[&](LoginRouting r) { cancelled=r.route==LoginRoute::Error && r.error=="superseded by signing out"; });
      if(kind==5) h.CreateNetwork("name","user","password","",done);
      h.Logout(); Drain(); SignIn(h,"B");
      if(kind==0) Respond(h.api_->password.front(),urnet::AuthLoginWithPasswordResult{std::nullopt,urnet::OptionalNetwork{std::string("A")},std::nullopt},outcome);
      if(kind==1) Respond(h.api_->code.front(),urnet::AuthCodeLoginResult{std::nullopt,"A"},outcome);
      if(kind==2 || kind==4) Respond(h.api_->login.front(),urnet::AuthLoginResult{std::nullopt,urnet::Network{"A"},std::nullopt,std::nullopt},outcome);
      if(kind==3) Respond(h.api_->verify.front(),urnet::AuthVerifyResult{std::nullopt,urnet::Network{"A"}},outcome);
      if(kind==5) Respond(h.api_->create.front(),urnet::NetworkCreateResult{std::nullopt,urnet::OptionalNetwork{std::string("A")},std::nullopt,std::nullopt},outcome);
      Drain(); ExpectB(h,"late auth API response"); Check(cancelled,"retired auth attempt answers cancellation");
      Check(unreachable==0,"retired auth failure cannot prompt a later account to change connection region");
    }
    {
      Fresh(); SdkHost h; bool cancelled=false;
      h.LoginWithCode("A",[&](AuthResult r) { cancelled=!r.ok; });
      h.api_->code.front()(urnet::AuthCodeLoginResult{std::nullopt,"A"},std::nullopt);
      h.Logout(); Drain(); SignIn(h,"B"); Drain();
      ExpectB(h,"logout between API guard and queued registration"); Check(cancelled,"queued old registration cancels");
    }
    for(int outcome=0;outcome<4;++outcome) {
      Fresh(); SdkHost h; bool cancelled=false;
      h.RegisterNetworkClient("A",[&](AuthResult r) { cancelled=!r.ok; },h.CaptureAuthEpoch()); Drain();
      auto old=h.registrations.front(); h.Logout(); Drain(); SignIn(h,"B");
      Respond(old,urnet::AuthNetworkClientResult{std::nullopt,"A-client"},outcome); Drain();
      ExpectB(h,"late network-client response"); Check(cancelled,"old client callback cancels");
    }
    for(int stage=0;stage<2;++stage) {
      Fresh(); SdkHost h; bool cancelled=false;
      if(stage==0) h.localState_->onJwt=[&] { h.Logout(); };
      else h.localState_->onClient=[&] { h.Logout(); };
      h.RegisterNetworkClient("A",[&](AuthResult r) { cancelled=!r.ok; },h.CaptureAuthEpoch()); Drain();
      if(stage==1) { h.registrations.front()(urnet::AuthNetworkClientResult{std::nullopt,"A-client"},std::nullopt); Drain(); }
      Check(cancelled && h.signedOut_ && !h.hasClientJwt_,"logout during persistence never republishes login");
      Check(h.localState_->jwt.empty() && h.localState_->client.empty(),"tail clears the retired synchronous write");
      SignIn(h,"B"); ExpectB(h,"later account survives persistence race");
    }
    {
      Fresh(); SdkHost h; bool bDone=false;
      h.Logout();
      h.RegisterNetworkClient("B",[&](AuthResult r) { bDone=r.ok; },h.CaptureAuthEpoch());
      Main();
      Check(h.registrations.empty(),"new registration waits behind local logout");
      Drain();
      Check(!h.registrations.empty(),"deferred new account is admitted after tail");
      h.registrations.back()(urnet::AuthNetworkClientResult{std::nullopt,"B-client"},std::nullopt); Drain(); AsyncStorage();
      Check(bDone,"deferred account completes"); ExpectB(h,"tail never erases a later account");
    }
    {
      Fresh(); SdkHost h; h.Logout(); size_t queued=workerTasks.size(); h.Logout();
      Check(h.cryptoInvoice_->signOuts==1,"sign-out immediately clears the invoice once, before the worker tail");
      Check(!h.NetworkServerChangeAllowed(),"space switch cannot replace the old logout's local state");
      Check(workerTasks.size()==queued,"duplicate logout joins one tail");
      bool cancelled=false; h.LoginWithCode("A",[&](AuthResult r) { cancelled=!r.ok; }); h.Logout();
      Check(workerTasks.size()==queued,"retiring a new attempt still joins the running tail");
      Check(h.cryptoInvoice_->signOuts==1,"joining a running logout never clears another invoice");
      h.api_->code.front()(urnet::AuthCodeLoginResult{std::nullopt,"A"},std::nullopt); Drain();
      Check(cancelled && h.signedOut_,"logout while signed out retires a new pending auth attempt");
      Check(h.NetworkServerChangeAllowed(),"space switch can retry after tail settles");
    }
    {
      Fresh(); SdkHost h; MainWindow ui{h,{}}; ui.InstallAuthHandler();
      h.RegisterNetworkClient("A",[](AuthResult){},h.CaptureAuthEpoch()); Drain();
      h.registrations.front()(urnet::AuthNetworkClientResult{std::nullopt,"A-client"},std::nullopt); Main();
      Step(workerTasks); Step(mainTasks); // success event is now queued for GTK
      h.Logout(); Drain();
      Check(std::find(ui.shown.begin(),ui.shown.end(),true)==ui.shown.end(),"queued old auth event cannot switch GTK back to logged in");
    }
    for(int newer=0;newer<2;++newer) {
      Fresh(); SdkHost h; bool signedIn=false;
      if(newer) h.Logout();
      h.CreateInstantAccount("",[](InstantAccount){});
      h.api_->create.front()(urnet::NetworkCreateResult{std::nullopt,urnet::OptionalNetwork{std::string("instant")},std::nullopt,std::string("phrase")},std::nullopt);
      if(!newer) h.Logout();
      Drain(); h.ConfirmInstantAccount([&](AuthResult r) { signedIn=r.ok; }); Drain();
      if(newer) {
        Check(!h.registrations.empty(),"new held instant account survives old tail");
        h.registrations.back()(urnet::AuthNetworkClientResult{std::nullopt,"instant-client"},std::nullopt); Drain();
        Check(signedIn,"new held instant account confirms");
      } else Check(!signedIn && h.registrations.empty(),"retired instant token cannot register");
    }
    for(int kind=0;kind<2;++kind) {
      Fresh(); SdkHost h;
      h.walletAuthEpoch_=h.CaptureAuthEpoch(); h.walletFlows_.Begin(); h.walletAuthDone_=[](AuthResult){};
      if(kind==0) h.AuthLoginWithSso("provider","A"); else h.AuthLoginWithWallet("A","signature","message","solana");
      h.Logout(); Drain();
      bool bCalled=false; h.walletAuthEpoch_=h.CaptureAuthEpoch(); h.walletFlows_.Begin(); h.walletAuthDone_=[&](AuthResult){bCalled=true;};
      h.api_->login.front()(urnet::AuthLoginResult{std::nullopt,urnet::Network{"A"},std::nullopt,std::nullopt},std::nullopt); Drain();
      Check(!bCalled && static_cast<bool>(h.walletAuthDone_),"old wallet/SSO API callback cannot steal new callback slot");
    }
    for(int stage=0;stage<2;++stage) {
      Fresh(); SdkHost h; bool failed=false;
      h.hasClientJwt_=false; h.signedOut_=true;
      if(stage==0) h.localState_->failJwt=true; else h.localState_->failClient=true;
      h.RegisterNetworkClient("A",[&](AuthResult r){failed=!r.ok && r.error=="could not save session";},h.CaptureAuthEpoch()); Drain();
      if(stage==1) { h.registrations.front()(urnet::AuthNetworkClientResult{std::nullopt,"A-client"},std::nullopt); Drain(); }
      Check(failed && h.signedOut_ && !h.hasClientJwt_,"persistence exception cannot publish a sign-in");
    }
    std::printf("%d actual-method auth/session checks passed\n",checks);
    return 0;
  } catch(const std::exception& e) { std::fprintf(stderr,"FAIL after %d checks: %s\n",checks,e.what()); return 1; }
}
