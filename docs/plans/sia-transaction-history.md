# Plan: show Siacoin (SC) transaction history in the desktop wallet

> **Status:** not started. Activation and swaps work (`9882d3688`). Withdrawal
> (sending SC) was fixed in two follow-up sessions (see "Prior work" below).
> Viewing past SC transactions — the subject of this plan — has not been
> touched: the wallet still tells KDF not to track SC history at all.

## tl;dr for whoever implements this

Two flags are wrong, in one file, `src/core/atomicdex/services/kdf/kdf.service.cpp`:

1. `with_tx_history` is hardcoded to `false` when activating an SC coin
   (`prepare_enable_coin_task`, the `coin_info.is_sia_family` branch). It must
   be `true`. This is the actual blocker — see "Root cause" below.
2. `uses_v2_history()` returns `true` for `CoinType::SIA`. It should return
   `false` — SC history should ride the same legacy `my_tx_history` v1 path
   every UTXO coin already uses, not the mmrpc-2.0 v2 path. See "Design
   decision: v1, not v2" below for why; this is not just tidiness, it's a
   real cross-backend compatibility difference.

Everything else needed to render the result — response parsing, the QML
history view, fee-shape handling — already exists and needs no new code,
per the investigation in this document. This is a small, well-contained fix
if the two flags above are the only thing touched; treat any additional
scope creep as a sign the investigation below missed something and stop to
re-check rather than pushing through it.

## Prior work (what's already done, and by whom)

- **SC activation and swaps** — `9882d3688` "Adds SIA activation and swap
  functionality", authored by `smk762`, Nov 2024. Not part of any traceable
  Claude Code session (no `Co-Authored-By` trailer; predates this repo's
  current session history on this machine).
- **SC withdraw (sending) fixes** — two separate, Claude-authored fixes, both
  with `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` trailers but
  **no recoverable session name** (Claude Code does not tag commits with a
  session identifier, and this machine has no local session history for this
  repo predating this plan — these were very likely run from a different
  machine/environment). Identify them by commit instead:
  - `bfa2167b6` "fix(wallet): stop swallowing SC withdrawals into a permanent
    busy spinner" (branch `fix/sia-withdraw-stuck`).
  - `ce7f50537` "fix(kdf-api): stop crashing on a completed SC withdraw's
    fee_details" (branch `fix/sia-withdraw-fee-details-crash`). This one
    matters directly for this plan — see "Fee-shape parsing" below.
  - Also relevant: `d1bf5775b` "fix: make the crash log usable, and stop
    returning garbage tasks" hardened the same `is_sia_family` activation
    branch this plan touches against a missing-server-list crash. Read it
    before editing that branch so the new change doesn't reopen it.
- **SC transaction history, backend side** — `kdf-reloaded-public` commit
  `3a078bb91` "feat(sia): report transaction history from walletd events",
  2026-08-13, Claude-authored (`Co-Authored-By: Claude Sonnet 5`), implementing
  **CRD ch.53** (`kdf-reloaded-public/CHANGELOG.md`, "Unreleased → Added").
  This is a different repository from the one this plan lives in — **do not
  implement this plan inside `kdf-reloaded-public`**, per your own framing:
  it is desktop-wallet work, not KDF work. The backend feature is complete
  and released; nothing here should touch `kdf-reloaded-public`.

None of the above is a "desktop wallet shows SC history" plan — this document
is that plan, and as far as this investigation found, it did not exist before
now.

## Root cause

`kdf-reloaded-public`'s `CHANGELOG.md` states the exact activation-time
behavior plainly:

> Tracking is enabled by the existing `tx_history` activation flag (default
> `false`, ch.46 §46.1.2); a coin activated without it answers history
> requests successfully with an empty list and the not-enabled sync status.

The desktop wallet activates every SC coin with that flag forced to `false`:

```cpp
// src/core/atomicdex/services/kdf/kdf.service.cpp, prepare_enable_coin_task,
// coin_info.is_sia_family branch:
t_enable_sia_coin_request request{
    .coin_name            = coin_info.ticker,
    .server_url           = sia_urls->at(0),
    .with_tx_history      = false}; // NotSupportedFor
```

The `// NotSupportedFor` comment was accurate when SC activation was first
added (Nov 2024) — the backend genuinely didn't support it then, and
`my_tx_history_v2_rpc` used to reject SC with a `NotSupportedFor` error,
confirmed in the same CHANGELOG entry ("the mmrpc-2.0 `my_tx_history` rejected
SC outright with `NotSupportedFor`"). It is **stale** now: CRD ch.53 shipped
Sia history support, and leaving this flag `false` means an SC coin's history
loop never starts on the backend (`SiaCoinBuilder::build` in
`kdf-reloaded-public/mm2src/coins/siacoin/siacoin_helpers.rs` maps
`tx_history: false` straight to `HistorySyncState::NotEnabled`, and
`process_history_loop` returns immediately when it sees that state —
`kdf-reloaded-public/mm2src/coins/siacoin/siacoin_history.rs`). The request
never errors, so this doesn't crash or log anything — it just silently, permanently
returns nothing, which matches "sicoin transactions still don't show up" with
no error to search for.

## Design decision: v1, not v2

`kdf.service.cpp::uses_v2_history()` currently returns `true` for
`CoinType::SIA`, alongside ZHTLC and Tendermint. This routes SC's history
fetch through the mmrpc-2.0 envelope (`prepare_batch_balance_and_tx`).

**Recommendation: don't.** Route SC through the same legacy v1 `my_tx_history`
path every UTXO coin already uses (i.e. exclude `CoinType::SIA` from
`uses_v2_history()`, and let `prepare_batch_balance_and_tx`'s existing
default `method = "my_tx_history"`, `requires_v2 = false` apply). Two
independent reasons, both backed by source, not just style:

1. **Cross-backend compatibility.** `kdf-reloaded-public/RELOADED_VS_GLEEC.md`
   records this exact divergence: *"GLEEC KDF serves SC history through the
   legacy tier-1 `my_tx_history` only, and rejects an SC request on the
   mmrpc-2.0 method with the not-supported error. Reloaded accepts SC on both
   tiers."* This wallet's `origin`/`upstream` remotes
   (`takologi-contrib/komodo-wallet-desktop`, `cipig/komodo-wallet-desktop`)
   don't pin it to one specific KDF backend — a user could point it at either.
   The v1 path works against **both**; the v2 path only works against
   `kdf-reloaded`. Confirm this assumption still holds (which KDF backend(s)
   this wallet is actually meant to support) before implementing — see open
   question 1 below — but if it does, v1 is the only choice that doesn't
   silently break for one backend.
2. **It's already fully wired and proven.** `kdf-reloaded-public`'s legacy
   `my_tx_history` handler (`mm2src/coins/lp_coins_ops.rs::my_tx_history`) is
   coin-generic — it calls `coin.load_history_from_file()`, a trait method
   every `MmCoin` including `SiaCoin` implements, with no per-coin-type branch
   to add. On the desktop-wallet side, `rpc.tx.history.cpp`'s `to_json`
   already branches on the `mmrpc` envelope and emits a correct **v1** request
   (flat `{"coin", "limit"}`, no wrapper) when `requires_v2` is false — this
   code path is exercised today by every UTXO coin. **Zero new
   request/response-parsing code is needed** if SC uses v1; using v2 instead
   would require *adding* a real method-name branch (analogous to ZHTLC's
   `method = "z_coin_tx_history"`) that does not exist today for SC — i.e.
   staying on v2 is strictly more work for a backend-compatibility downgrade.

If you disagree with this recommendation after checking open question 1,
the alternative is: keep `uses_v2_history()` returning `true` for SIA, and
add an explicit method-name branch for it in `prepare_batch_balance_and_tx`
(there currently isn't one — SC silently reuses the default `"my_tx_history"`
method name *with* `requires_v2=true`, which is not a real KDF method
combination and was never actually exercised, since `with_tx_history=false`
mask the fact it would never receive data anyway). Don't leave this
half-configured either way.

## Response parsing — already works, verify it does

- `transaction.data.cpp::from_json(transaction_data&)` and the shared
  `fees_data::from_json` were already extended for Sia's fee shape
  (`{"type":"Sia",...,"total_amount":...}` instead of `"amount"`) by
  `ce7f50537`. This same parser is used for both withdraw responses and
  `my_tx_history` responses (`tx_history_answer_success::transactions` is a
  `std::vector<transaction_data>`) — so this should already work for history
  once real SC transaction JSON reaches it. **Verify this with the
  fixture-based test below rather than assuming it** — the earlier fix was
  written against a withdraw response, not a history response, and the two
  are structurally the same but were never actually cross-checked.
- `transaction_data::from_json` does not currently read `internal_id` at all
  (commented out, applies to every coin, not Sia-specific) — irrelevant here.
- The new `transaction_type` field KDF Reloaded added
  (`SiaV1Transaction`/`SiaV2Transaction`/`SiaMinerPayout`,
  `kdf-reloaded-public/mm2src/coins/lp_coins_types.rs`) is `#[serde(default)]`
  on the Rust side and isn't read by `transaction_data::from_json` at all on
  the desktop-wallet side — an unrecognized JSON field is silently ignored by
  `nlohmann::json`'s `.at()`/`.get_to()` pattern used here, so its presence is
  harmless. Whether the UI should ever *display* it (e.g. label a miner
  payout differently from a transfer) is a separate, optional follow-up —
  out of scope for making history visible at all; note it as a nice-to-have,
  don't block on it.
- The QML view (`atomic_defi_design/Dex/Wallet/Transactions.qml`,
  `TransactionDetailsModal.qml`) has no coin-type branching at all — it's
  generic, driven by whatever the C++ model exposes. No QML changes expected.
- Block-explorer links come from each coin's `coins.cfg` entry
  (`explorer_tx_url`/`explorer_address_url`), not per-coin-type code. Confirm
  SC's config entry has these set — if not, that's a coin-list config fix,
  not a code fix, and out of scope for this plan.

## Implementation steps

1. In `kdf.service.cpp`, flip the SC activation request:
   `with_tx_history = true` (remove the stale `// NotSupportedFor` comment,
   replace with a comment pointing at CRD ch.53 / this plan, so a future
   reader doesn't wonder why it's `true` and doesn't need this file to find out).
2. Remove `CoinType::SIA` from `uses_v2_history()` (per the design decision
   above), or implement the missing v2 method-name branch if open question 1
   resolves the other way. Either way, `uses_task_activation()` **stays
   unchanged** — SC still activates via the task-based flow; only the
   *history-fetch* method selection changes. These are two independent
   predicates; don't conflate them.
3. **Extract `uses_v2_history` and `uses_task_activation` into free functions**
   (e.g. a small header/cpp pair, or `static` functions in an anonymous
   namespace next to `kdf_service`'s implementation) taking `const
   coin_config_t&` and returning `bool`, with no dependency on a live
   `kdf_service` instance. They already have this exact signature as member
   functions and touch no instance state — this is a mechanical, low-risk
   lift, not a redesign. Do this **specifically** so the automatic check in
   step 4 can call them without constructing a `kdf_service` (which requires
   an `entt::registry&`/`ag::ecs::system_manager&` — real infrastructure a
   unit test shouldn't need to pull in for what is otherwise a two-line pure
   function). Keep `kdf_service::uses_v2_history`/`uses_task_activation` as
   thin wrappers calling the free functions, so no call site elsewhere in the
   class needs to change.
4. Add the CI test target described below.
5. Manual verification per the human-operator checklist below.

## Automatic checks (CI)

This repo's only current CI (`​.github/workflows/atomicdex-desktop-ci.yml`) is
a build matrix (ubuntu/osx/windows × release/debug) — there is no unit-test
step today, and no existing `tests/` directory. Add one, scoped narrowly:

- **New CMake target** (e.g. `komodo_wallet_unit_tests`) covering only the
  pure JSON (de)serialization layer touched by this change — no Qt, no ECS,
  no vcpkg GUI deps. Confirm before committing to this that the following
  translation units genuinely have no transitive Qt dependency (a quick
  `grep -l QT\|Qt` import check on each, plus a standalone compile attempt,
  since this plan's investigation only read them, it did not compile them
  in isolation):
  - `src/core/atomicdex/api/kdf/transaction.data.cpp`
  - `src/core/atomicdex/api/kdf/rpc.tx.history.cpp`
  - `src/core/atomicdex/api/kdf/rpc_v2/rpc2.task.enable_sia_coin.init.cpp`
  - the new free-function file from step 3 above.
  If any of them turn out to pull in Qt transitively, split the pure JSON
  logic out into a dependency-free file rather than abandoning the test target.
- **Test cases**, each against a literal JSON fixture (inline string or a
  small `tests/fixtures/*.json` file — prefer a real captured response if one
  can be obtained from a `kdf-reloaded` testnet walletd during manual testing,
  falling back to a hand-built fixture matching the Rust struct shapes cited
  in this document if not):
  1. `enable_sia_coin_request::to_json` on a representative request emits
     `params.tx_history == true` (regression guard for the root-cause fix —
     this is the one flag a future refactor is most likely to silently flip
     back).
  2. `tx_history_request::to_json` with `requires_v2=false` (the SC case)
     produces the **flat v1 shape** — no `"params"` wrapper, no `"mmrpc"`
     key — asserting this exact shape as a regression guard for the v1/v2
     design decision.
  3. `transaction_data::from_json` parses a fixture SC transaction (Sia-shaped
     fee with `total_amount`, a plain string `tx_hash`, and each of the three
     new `transaction_type` values in turn) without throwing, and produces
     the expected `fees_data.normal_fees` value.
  4. `tx_history_answer::from_json` parses a full fixture v1 `my_tx_history`
     response (`result.transactions` containing the case-3 fixture plus
     `sync_status.state` of each of `NotEnabled`/`NotStarted`/`InProgress`/
     `Finished`) into a fully populated `tx_history_answer_success` with the
     right `transactions.size()`.
  5. The two extracted free functions from step 3: `uses_v2_history` returns
     `false` and `uses_task_activation` returns `true` for a `coin_config_t`
     with `coin_type == CoinType::SIA`, and unit-test their unchanged
     behavior for at least one other `CoinType` (ZHTLC) as a
     don't-break-existing-coins guard.
- **Wire the new target into the existing GitHub Actions workflow** as an
  additional fast job (it does not need the Qt/vcpkg/MSVC setup steps the
  build matrix uses, so it should be able to run in well under a minute on
  a bare `ubuntu-latest` runner with only a C++ toolchain and CMake) rather
  than folding it into the existing platform-build jobs.

## Human-operator checklist (manual, needs a real KDF + real SC activity)

None of this is automatable from this repo alone — it needs a live KDF
instance (ideally `kdf-reloaded`, per the recommendation above; also try
GLEEC KDF if reachable, to confirm the cross-backend claim this plan's design
decision rests on) and either testnet SC funds or a wallet with prior SC
activity.

1. **Fresh SC activation, wallet with pre-existing history.** Activate an SC
   coin whose address already has confirmed transactions from before this
   change. Confirm they appear in the wallet's transaction/history view
   within a reasonable time (the backend's `process_history_loop` is a
   background pass, not instant — give it the poll interval described in
   `kdf-reloaded-public`'s changelog, and don't file "still empty" as a bug
   until you've waited past that).
2. **Empty-history wallet.** Activate a freshly generated SC address with no
   prior transactions. Confirm the history view shows an empty list cleanly —
   no error, no spinner stuck forever, no crash. (This is the exact class of
   bug `d1bf5775b` and `bfa2167b6` fixed elsewhere in this activation path —
   re-check nothing regresses here.)
3. **Withdraw-then-appears round trip.** Send a small SC withdrawal (exercises
   the `bfa2167b6`/`ce7f50537` fixed path), then confirm that same transaction
   shows up in the history view once confirmed — ties this plan's fix
   together with the two prior withdraw fixes end to end, which has never
   been verified as a single flow.
4. **Sync-status visibility.** If the UI surfaces `sync_status` anywhere
   (a "syncing history" indicator, etc.), confirm it transitions sensibly
   (`NotStarted` → `InProgress` → `Finished`) rather than staying permanently
   in one state — this is newly-reachable UI state for SC specifically, since
   before this fix SC could only ever report `NotEnabled`.
5. **Pagination**, if the test wallet has more transactions than one page's
   `limit` (currently 250 per `prepare_batch_balance_and_tx`'s default) —
   confirm scrolling/paging still works and doesn't duplicate or drop rows.
6. **Backend cross-check** (only if GLEEC KDF is reachable for testing):
   activate the same SC wallet against a GLEEC KDF instance instead of
   `kdf-reloaded`, confirm history still works (validates the v1-not-v2
   design decision actually holds against the other backend, not just in
   the source-reading this plan is based on).
7. **Regression pass on the two prior SC fixes** — re-run their original
   repro steps (stuck withdrawal / fee_details crash) to confirm this
   change doesn't reopen either.

## Open questions — resolve before or during implementation

1. **Which KDF backend(s) does this wallet actually need to support?** The
   v1-vs-v2 design decision above assumes "possibly either GLEEC or
   Reloaded" from the remote names and `RELOADED_VS_GLEEC.md`'s framing as a
   real compatibility concern, but this plan did not find an explicit
   statement of this project's supported-backend policy. If it turns out
   this wallet is `kdf-reloaded`-only in practice, the v2 path becomes a
   valid (if more work) alternative — re-read the "Design decision" section's
   alternative-path note in that case.
2. **`limit = 250` default** for non-ZHTLC v2 history in
   `prepare_batch_balance_and_tx` — confirmed this is the existing default
   for every coin already using this path, not something to change for SC
   specifically. No action needed, noted here only so it isn't mistaken for
   an SC-specific oversight during review.
3. **Should `transaction_type` (miner payout vs. v1/v2 transfer) be surfaced
   in the UI** as a distinct label/icon? Explicitly out of scope for this
   plan (making history visible at all is the goal), but flagged here as the
   natural next increment once this lands.
