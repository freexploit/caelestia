## Context

The managed notification path has three layers that each record "error" differently:

```
Plasma plasmashell (presenter applet)          Caelestia shell (quickshell)          caelestia-activities (Python)
┌──────────────────────────────┐   D-Bus   ┌──────────────────────────────┐  stdout/IPC  ┌──────────────────────────┐
│ NotificationPresenter        │◄─────────►│ PlasmaNotifications client   │◄────────────►│ Coordinator              │
│  m_error  (single QString)   │           │  m_error / m_ready / m_active│              │  journal["error"]        │
│  Snapshot().supported        │           │  fail() → stop()             │              │  failed_activity         │
└──────────────────────────────┘           └──────────────────────────────┘              └──────────────────────────┘
```

Observed failure (2026-11-03 11:10–11:15):

1. A reply-capable notification reached the leased shell while the user was in the Caelestia Activity.
2. `NotificationPresenter::describe()` (`presenter.cpp:263`) treated `HasReplyActionRole` as a presenter capability failure and set the process-global `m_error`.
3. `Snapshot().supported` is `supported() && m_error.isEmpty()` (`presenter.cpp:94`), so every later snapshot reported `supported: false`; there is no reset site in the file.
4. The shell's `Notify` path saw a description without an `id` and called `fail()`, unregistering the watcher and dropping readiness.
5. `Coordinator.preflight()` (`caelestia_activities.py:222`) hard-failed, `enter()` aborted, `leave()` restored surfaces, and the failure string was appended to the journal.
6. `failed_activity` then suppressed retries while the same Activity stayed current, and `save()` never clears `error`, so `activities.json` kept a stale failure indefinitely.

The journal file itself is healthy: `version: 1`, `state: baseline`, empty ownership records. The string is the only problem, and it is a true snapshot of a still-latched presenter.

Verified Activity mapping (2026-11-03, `kactivitymanagerdrc` + `plasma-org.kde.plasma.desktop-appletsrc`):

| UUID | display name | role | owned containments |
|---|---|---|---|
| `58866ae6-d479-4752-9546-0af404bd48d0` | `Another` | baseline, plasma-manager desktops | 415 (screen 0), 416 (screen 1) |
| `79a4e569-e86f-4132-9461-fb18672aa7ba` | `Caelestia` | target, managed shell | 479 (screen 1), 480 (screen 0), each with `org.caelestia.notification-presenter` |

Current Activity at investigation time was `Caelestia`. The panels (485, 511) and the system tray have an empty `activityId`, so they are global to both Activities; that is why the coordinator hides them through the Panel Colorizer instead of relying on Activity scoping.

The `("Default", pre-existing)` label in `caelestia.nix` and older comments is stale: the baseline Activity was renamed to `Another`. Neither the coordinator nor the presenter uses Activity display names — the baseline is referenced by UUID and the target is resolved from `activity-identity.json` or an exact name match — so this is documentation drift, not a functional defect.

## Goals / Non-Goals

**Goals:**

- A single notification can never change the presenter's advertised capability.
- Per-notification problems degrade that notification and are logged; the bridge, lease, and shell stay alive.
- The coordinator distinguishes capability failure from notification outcomes without string matching.
- A transient entry failure recovers without an Activity switch, with bounded backoff.
- `activities.json` `error` describes the current state, not the first failure ever recorded.
- Reproduce the 2026-11-03 chain in the lab and keep it green.

**Non-Goals:**

- Implementing inline reply in the Caelestia notification UI.
- Changing Plasma's DND policy (the snapshot's `inhibited: true` is the user's DND setting, unrelated to this latch).
- Changing notification popup policy, sound policy, or history semantics.
- Migrating the journal schema (stays version 1).

## Decisions

### 1. Split presenter state into capability error vs per-call notification outcome

`NotificationPresenter` keeps `m_error` for capability/lifecycle failures only (attach, process ownership, registration, lease). `describe()` stops writing `m_error`; its per-notification outcome lives in the return value.

- `Describe(id)` returns a map that either carries the presentation fields or `{declined: true, reason: "absent" | "unreadable"}`.
- `Existing()` omits declined entries and returns the rest.
- `PolicyChanged` is emitted only for capability/lease transitions.

**Alternative rejected:** clear `m_error` at the start of every `Snapshot()`. It makes the capability flag self-healing but hides genuine lease loss, races with `PolicyChanged`, and changes `Snapshot` from a read into a state mutation.

### 2. Degrade per notification before declining

Notification fields are independent; only a missing model row makes a notification unusable.

| Condition | Behavior |
|---|---|
| `HasReplyActionRole` | present normally, no reply affordance (the wire format already carries no reply action) |
| action names/labels mismatch | present with sender actions omitted |
| image encoding failure | present without image |
| row absent from model / wrong type | decline with `reason: absent` |
| malformed D-Bus reply | capability/transport error (unchanged) |

**Alternative rejected:** keep rejecting reply-capable notifications but only locally. It still makes every Slack/Beeper/Thunderbird notification invisible while the shell is leased, which is a worse outcome than an action-less popup.

**Trade-off accepted:** users cannot reply from the Caelestia popup. No current UI offers reply, so this does not remove an existing affordance; it only stops a notification from taking down the bridge.

### 3. Shell fatal path is limited to transport/ownership failures

`PlasmaNotifications::Notify` and `setActive`/`Existing` classify the presenter reply:

- `declined` → log `notification <id> declined: <reason>`, skip it, keep `m_ready`/`m_active`/registrations.
- delivered → existing path (`deliver`), unchanged except the id check no longer conflates decline with capability failure.
- D-Bus error, owner change, registration failure, policy `supported: false` → existing `fail()` path.

Delivered ids remain the only entries in `m_entries`, so `Present`/`Eligible`/`Dismiss` behave as today.

### 4. Coordinator classifies by contract, retries with bounded backoff

`preflight()` keeps its single presenter gate (`Snapshot().supported`), which now only reflects capability. It does not inspect error strings or notification lists.

Entry retry becomes a state machine instead of `failed_activity`:

```
                   ┌──────────── transient preflight failure ────────────┐
                   ▼                                                     │
 idle ──target current──► entering ──success──► caelestia ──leave──► baseline/entering
                            │  ▲
                            │  └── wake (presenter/plasmashell owner change, activity change)
                            └── persistent failure ──► failed(until wake/activity change)
```

- Transient = a preflight capability failure when the presenter/plasmashell service is still owned and the Activity manager is reachable.
- Backoff 2 s → 5 s → 15 s → 60 s, reset on success, Activity change, or a watched `NameOwnerChanged` (the coordinator already subscribes to the presenter, plasmashell, KWin, and activity services).
- Persistent = configuration/identity failure (missing baseline, identity mismatch), shell exiting during startup, shortcut registration failure, non-preflight entry failure. Stop retrying until a wake event.

Log one line per failure-state change, not per retry, so backoff does not flood the journal.

### 5. Journal error clearing uses a sentinel

`save()` gains a `_UNSET` sentinel: calling `save(state=...)` leaves `error` untouched; `save(..., error=None)` removes the key; `save(..., error="...")` sets it. `enter()` starts with `save("entering", error=None)` and ends with `save("caelestia", error=None)`. The failing path keeps `save(error=str(exc))` after `leave()`.

Schema stays `version: 1`; a stale `error` in an existing file is simply cleared on the next attempt.

### 6. Lab reproduction

`nix/lab/clients.py` `notification` gains `--reply`, which adds KDE's inline-reply hint to the real `Notify` call. A new `notifications-reply` scenario asserts:

1. presenter `Snapshot().supported` stays `true` and `error` is empty after the notification;
2. `managed/status` stays `ready` and `active`;
3. a second plain notification is still delivered;
4. `activities.json` has `state: caelestia` and no `error`.

Coordinator retry gets a fast unit test with a fake `dbus-next` bus (presenter unavailable, then available) asserting entry succeeds without an Activity switch and the stale error disappears.

## Risks / Trade-offs

- **Reply affordance is absent** for reply-capable notifications → acceptable while no reply UI exists; document it in the managed-notifications spec and the module comments.
- **Dropping sender actions on a metadata mismatch** can hide a usable button → log the notification id and mismatch; mismatch is already a sender/model bug.
- **Retry loop against a permanently broken installer** → cap at 60 s, only log on state change, and stop on non-transient failures.
- **Presenter and coordinator skew** (old coordinator against new presenter, or vice versa) → they are built and pinned from the same flake rev; note in the proposal impact and keep the coordinator from parsing `error` strings.
- **Existing poisoned plasmashell** stays poisoned after deployment until it restarts → migration includes one explicit plasmashell restart; the coordinator then recovers through its normal wake path.
- **`Existing()` replay of a reply-capable history entry** now degrades every entry instead of failing once → the notification reappears each entry without reply; acceptable and test-covered.

## Migration Plan

1. Merge the presenter/shell/coordinator changes; rebuild the flake.
2. Move the `caelestia-kde` input pin in `~/.config/home-manager` and run `home-manager switch`.
3. Restart `plasma-plasmashell` once so the currently latched presenter object is replaced; restart `caelestia-activities.service` (or switch Activity away/back) to clear the in-memory `failed_activity`.
4. Verify: `Snapshot` returns `supported: true`; `systemctl --user restart caelestia-activities`; enter Caelestia; `activities.json` shows `state: caelestia` with no `error`.
5. Rollback: revert the flake pin. No on-disk migration is needed; version-1 journals from either build are accepted.

## Open Questions

- Should a reply-capable notification be routed to native Plasma popups instead of degraded? Resolved during implementation: no. The lease is global; the notification is delivered with the `inline-reply` action filtered out, and the bridge stays alive.
- Should declined notifications get a one-time user-visible hint ("reply not supported")? Deferred; the categorized warning is the current surface.
- Should the journal record attempt counters/timestamps for diagnostics? Deferred to keep the schema stable.
- The user's Plasma DND is configured until 2027-01-20 (`plasmanotifyrc [DoNotDisturb] Until`). It suppresses popups independently of this fix; whether the long expiry is intentional is the user's decision and remains unconfirmed.
