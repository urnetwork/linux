# Destination-page implementation contract (Windows-parity wave)

Every Windows-parity destination page is built against this contract so seven
pages come out of one system. The specs in this directory are the design
source of truth; the code sources of truth are:

- `app/src/PaneKit.hpp` — the component kit (urnw::kit). USE IT for every
  pane, group header, row, table, status/copy/metric/settings surface. Do not
  hand-build a row species the kit already has.
- `app/src/UrTheme.cpp` — the CSS vocabulary (`.ur-pane*`, `.ur-group*`,
  `.ur-key/.ur-value/.ur-row-title/.ur-row-note/.ur-col-header`,
  `button.ur-pane-row`, `button.ur-pane-primary/secondary`, `.ur-card`,
  `.ur-stat-*`, `.ur-vrule`). Do not invent new hex values — every color in
  the specs already exists as a class or in `Ui.hpp`'s Rgba palette.
- `app/src/I18n.hpp` — `T_(key, english)` / `TN_` / `Format`. Every
  user-facing string uses the exact store key + English from the spec
  (Adv-fallback keys use `T_` the same way; the English renders until the
  store grows the key).
- `app/src/SdkHost.hpp` — the SDK surface. `app/src/Ui.hpp` — PostToMain,
  palette, MakeCard/MakeChip helpers, EnsureDrawerCss.
- `docs/parity/sdk-wiring.md` — which SdkHost/urnet call backs each behavior.

## The page class

```cpp
// src/<Name>Page.hpp / .cpp — NEW FILES ONLY. Touch nothing else.
namespace urnw {
class <Name>Page : public Gtk::Box {
 public:
  explicit <Name>Page(SdkHost& host);
  void Load();                    // nav-select + auth-change API loads
  void SetAdvancedMode(bool on);  // only if the spec uses it
  void ApplyBreakpoint(int widthDip);  // the spec's pane-fold table
 private:
  SdkHost& host_;
  std::shared_ptr<uint64_t> epoch_ = std::make_shared<uint64_t>(0);  // stale-async guard
  ...
};
}
```

## Rules

1. **Panes** come from `kit::MakePane`, separated by `kit::MakePaneVRule()`;
   the page is a horizontal Box of them. Fold panes per the spec's
   ApplyBreakpoint thresholds by `set_visible` on pane roots.
2. **No session may crash or blank anything.** The preview harness runs with
   NO jwt and NO device: every SdkHost read already degrades (nullopt/empty);
   every async field renders one of Loading / Loaded / Failed / Empty as the
   spec distinguishes them (`kit::MakePaneEmptyLine`, `kit::MakeEmptyState*`).
   401-vs-empty must not look identical where the spec says so.
3. **Async discipline**: SDK/API completions land on SDK threads — marshal
   with `PostToMain`, guard with the epoch (bump in `Load()`), and use
   generation counters for debounced checks exactly as the spec states.
   Watchdogs/timeouts per spec (20s API, 180s browser bridge).
4. **Missing SDK surface**: if the spec requires a call that neither
   SdkHost.hpp nor the wiring table provides, render the real
   empty/unavailable state and mark the site `// TODO(sdk-wiring): <call>` —
   never invent a symbol, never fake data.
5. **Echo guards** on every toggle a load writes (the spec names them).
6. **a11y**: `kit::SetAccessibleLabel` on every icon-only button and every
   row-button; decorative marks are already hidden by the kit.
7. **Sheets** the page opens that already exist in the tree (UpgradeSheet,
   RedeemCodeSheet, DnsSheet, SplitRulesSheet, ContractsSheet,
   LocationsSheet, ProviderLocationsSheet, PostQuantumIdentity — see
   `docs/parity/linux-reuse.md`) are REUSED: include and open them. Sheets
   that don't exist yet: build only if your spec fully defines them,
   following SeedphraseSheet.cpp's pattern (a modal brand-styled
   Gtk::Window); otherwise `// TODO(sheet): <name>` on the opening row.
8. **Syntax gate before you finish** (compile your TU only — never edit
   meson.build or any shared file):

```sh
distrobox enter urnet-noble -- bash -c 'cd /var/home/bazzite/Downloads/claude_sandbox_linux_app/urnetwork-linux/app && g++ -std=c++17 -fsyntax-only -Isrc -isystem third_party/urnetwork-sdk/amd64 $(pkg-config --cflags gtkmm-4.0 libadwaita-1 nlohmann_json) -DGETTEXT_PACKAGE=\"urnetwork\" src/<Name>Page.cpp'
```

Iterate until it exits clean. Warnings are fine; errors are not.
