## Why

A single reply-capable notification permanently disables the managed notification presenter. On 2026-11-03 a Slack notification (id 201) reached the leased shell, `NotificationPresenter::describe()` set the process-global `m_error` for the inline-reply case, and from then on `Snapshot().supported` was `false` for the lifetime of the plasmashell process. Every subsequent `preflight()` failed, `enter()` aborted, and `~/.local/state/caelestia/activities.json` was left in `state: baseline` with a stale `error` string. The user stayed on the Caelestia Activity with no managed shell, and the coordinator's `failed_activity` suppression prevented retry until the Activity changed.

The file was never corrupt: it accurately journals a real, still-live latch (`Snapshot` in plasmashell PID 3865 still reports `supported: false` with the same message). The defect is error scoping — per-notification data problems are being treated as backend capability failures — compounded by a write-only journal field and a retry policy that cannot recover inside the same Activity.

## What Changes

- Split the presenter's diagnostic state into two classes:
  - **Capability/fatal**: attach failures, non-authoritative process, duplicate registration/ownership, lease failures, lost/overridden popup binding. These keep gating `Snapshot().supported`, `Acquire()`, and the lease.
  - **Per-notification**: reply-action restrictions, action name/label mismatch, image encode failure. These MUST NOT alter `supported`, MUST NOT latch, and MUST be reported only to the caller describing that notification.
- `Describe(id)` MUST leave the notification model untouched and return either a describable map or an explicit per-notification decline (no global side effect). `Existing()` MUST skip undeclarable notifications rather than poisoning the presenter.
- The shell watcher MUST treat a per-notification decline as a skipped notification: log it, leave the Plasma watcher registered and the lease held, and keep delivering later notifications. A decline MUST NOT call `fail()`/`stop()`.
- The coordinator MUST distinguish a presenter capability failure from a per-notification decline; only the former blocks `preflight()`. A managed entry that fails transiently MUST be retried without requiring an Activity change, with bounded backoff.
- The coordinator journal MUST clear `error` after a successful `enter()` (and when a new attempt starts), so the field describes the current failure, not the first one ever seen.
- Add regression coverage that reproduces the failure chain with a synthetic reply-action notification and asserts the presenter/shell/coordinator stay healthy.

Out of scope: implementing inline reply in the Caelestia notification UI. Reply-capable notifications are deliberately declined; the change makes that decline local and recoverable. Also out of scope: changing Plasma's DND policy (the snapshot's `inhibited: true` comes from Plasma DND being enabled until 2027-01-20 and is unrelated to this latch).

## Capabilities

### New Capabilities

- `managed-notifications`: the presenter/shell contract that decides whether the scoped Plasma notification bridge is usable, how per-notification declines are reported, and how the shell behaves when one notification cannot be presented.
- `activity-coordinator`: the serialized owner of the Activity-scoped shell — preflight semantics, entry/exit journaling, retry/recovery behavior, and the meaning of each `activities.json` field.

### Modified Capabilities

None. `openspec/specs/` is empty in this repository, so both capabilities are introduced here.

## Impact

- `plasma/notification-presenter/presenter.{cpp,hpp}` — separate fatal vs per-notification error state; per-call decline reporting; `Snapshot`/`Acquire` gating.
- `shell/plugin/src/Caelestia/Services/plasmanotifications.{cpp,hpp}` — `Notify`/`Existing` handling of declines; no `fail()` on per-notification outcomes; watcher lifetime.
- `shell/services/Notifs.qml` — accept an empty/declined notification without dropping the managed provider.
- `src/lib/caelestia_activities.py` — preflight classification, retry/backoff after transient entry failure, journal error clearing.
- Tests/lab: presenter and coordinator regression coverage for the 2026-11-03 chain.
- Consumers: `~/.config/home-manager/caelestia.nix` consumes this flake input; no option changes are required, but the pinned rev must move after the fix.
- Observability: `activities.json` stops carrying stale errors; the shell log gains one line per declined notification instead of a dead notification bridge.
- Documentation: the baseline Activity is UUID `58866ae6…` but its current display name is `Another`, not the `Default` label used in `caelestia.nix` comments. Verified ownership: baseline containments 415/416, Caelestia containments 479/480 (with the presenter applet), panels global (empty `activityId`). The change corrects the label and keeps UUID-only resolution.
