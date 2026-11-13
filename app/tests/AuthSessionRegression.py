#!/usr/bin/env python3
"""Compile real auth/logout methods against deterministic external boundaries."""
from pathlib import Path
import argparse
import os
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--negative-control', choices=('epoch', 'async-logout'))
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root / 'src/SdkHost.cpp').read_text()
methods = []
for name in ('CaptureAuthEpoch', 'NoteApiError', 'StartLogin', 'LoginWithPassword', 'LoginWithCode',
             'LoginWithSeedphrase', 'VerifyCode', 'CreateNetwork', 'CreateInstantAccount',
             'ConfirmInstantAccount', 'HandleNetworkCreateResult', 'AuthLoginWithSso',
             'AuthLoginWithWallet', 'RegisterNetworkClient', 'FinishNetworkClientRegistration',
             'NoteAbandonedClient', 'RemoveAbandonedClientLocked', 'ServerSignOutLocked',
             'HasRefusedSignIn', 'RetryRefusedSignIn',
             'Logout', 'LogoutNow', 'NetworkServerChangeAllowed'):
    result_type = 'uint64_t' if name == 'CaptureAuthEpoch' else 'bool' if name in ('NetworkServerChangeAllowed', 'HasRefusedSignIn') else 'void'
    start = source.index(result_type + ' SdkHost::' + name + '(')
    end = source.index('\n}\n', start) + 3
    methods.append(source[start:end])
start = source.index('std::string NormalizeSeedphrase(')
methods.insert(0, source[start:source.index('\n}\n', start) + 3])
start = source.index('client_limit::Prompt ToClientLimitPrompt(const std::string&')
end = source.index('\n}\n', source.index('client_limit::Prompt ToClientLimitPrompt(const std::optional', start)) + 3
methods.insert(0, source[start:end])
start = source.index('constexpr auto kServerSignOutWait =')
end = source.index('\n}  // namespace', start)
methods.insert(0, source[start:end])
# The actual policy is exercised below; pin it before the space/SDK can change.
space_start = source.index('bool SdkHost::ApplyNetworkServer(')
space_body = source[space_start:source.index('\n}\n', space_start)]
assert space_body.index('if (!NetworkServerChangeAllowed()) return false;') < space_body.index('std::scoped_lock lock(mutex_);')
window = (root / 'src/MainWindow.cpp').read_text()
start = window.index('  host_.SetAuthStateHandler(')
end = window.index('\n  });', start) + len('\n  });')
methods.append('void MainWindow::InstallAuthHandler() {\n' + window[start:end] + '\n}\n')
actual = '\n'.join(methods)
if args.negative_control == 'epoch':
    actual = actual.replace('!AuthEpochCurrent(authEpoch)', 'false && !AuthEpochCurrent(authEpoch)')
if args.negative_control == 'async-logout':
    actual = actual.replace('localState_->logout();', 'asyncLocalState_->logout([](bool) {});')
fixture = (root / 'tests/AuthSessionRegression.cpp').read_text().replace('// @ACTUAL_METHODS@', actual)
with tempfile.TemporaryDirectory(prefix='urnetwork-auth-session-') as temp:
    cpp = Path(temp) / 'regression.cpp'
    exe = Path(temp) / 'regression'
    cpp.write_text(fixture)
    subprocess.run([*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17', '-pthread',
                    '-O0', '-I', str(root / 'src'), str(cpp), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], timeout=20)
    if args.negative_control:
        if result.returncode == 0:
            raise SystemExit('negative control unexpectedly passed')
        print('Negative control rejected:', args.negative_control)
    else:
        result.check_returncode()
