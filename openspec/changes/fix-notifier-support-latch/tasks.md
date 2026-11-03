## 1. Presenter error scoping

- [x] 1.1 In `plasma/notification-presenter/presenter.{hpp,cpp}`, split diagnostics: keep `m_error` for attach/ownership/lease capability failures only; remove every assignment in `describe()` (lines 254, 264, 273).
- [x] 1.2 Make `describe()` degrade per field: present reply-capable notifications without a reply affordance, omit mismatched sender actions, and omit an unencodable image; return `{declined: true, reason: "absent"}` for an invalid/absent model row instead of an empty map.
- [x] 1.3 Update `Describe()` and `Existing()` to surface and skip declined entries; ensure `PolicyChanged` is emitted only for capability/lease transitions, never for notification content.
- [x] 1.4 Emit one categorized warning per degraded or declined notification containing the id and condition.
- [x] 1.5 Build `plasma/notification-presenter` (CMake) and confirm the header/ABI change compiles with the vendored `notifications.h`.

## 2. Shell watcher resilience

- [x] 2.1 In `shell/plugin/src/Caelestia/Services/plasmanotifications.cpp`, handle `declined` before the `id` check in `Notify`: log, skip the entry, and do not call `fail()`/`stop()`.
- [x] 2.2 Make the `Existing()` replay in `setActive()` skip declined entries and keep the loop going; reserve `fail()` for D-Bus/transport errors.
- [x] 2.3 Restructure `deliver()` so a missing authoritative id is a per-notification decline while registration/lease state stays intact; keep `m_entries` limited to delivered ids.
- [x] 2.4 Confirm `Present`/`Eligible`/`Dismiss`/`invokeAction` behavior is unchanged for delivered entries; build the shell plugin and verify it loads.

## 3. Coordinator retry and journal

- [x] 3.1 In `src/lib/caelestia_activities.py`, add an `_UNSET` sentinel to `save()` so `error=None` removes the key, a plain `save(state=...)` leaves it untouched, and `save(error="...")` sets it.
- [x] 3.2 Clear the previous error at the start of `enter()` (`state: entering`, no `error`) and on success (`state: caelestia`, no `error`).
- [x] 3.3 Replace the `failed_activity` suppression with attempt state and bounded backoff (2 s → 5 s → 15 s → 60 s), reset on success, Activity change, or a watched `NameOwnerChanged` for the presenter/plasmashell/KWin services.
- [x] 3.4 Keep `preflight()` gated only on `Snapshot().supported`; do not parse the error string or inspect notification lists.
- [x] 3.5 Ensure `leave()` completes and the panel/tracker ownership records are consistent before any retry.
- [x] 3.6 Verify `--status`, `--recover`, and the ExecStopPost recovery path still work with a version-1 journal from the previous build.

## 4. Regression coverage

- [ ] 4.1 Extend `nix/lab/clients.py` `notification` with `--reply` to send a real `Notify` carrying KDE's inline-reply hint; confirm `HasReplyActionRole` is set in the guest.
- [x] 4.2 Add a `notifications-reply` scenario to `nix/lab/suite.py`: reply-capable notification, then assert presenter `Snapshot().supported` stays `true`, `managed/status` stays `ready`/`active`, a later plain notification is delivered, and `activities.json` has no `error`.
- [x] 4.3 Add a coordinator test with a fake `dbus-next` bus where the presenter is unsupported once and then supported: assert `enter()` succeeds without an Activity switch, the stale `error` disappears, and the retry delay is bounded.
- [x] 4.4 Add a journal test for stale-error clearing on a new attempt and on a successful entry, including an old journal that already has `state: baseline` + `error`.
- [ ] 4.5 Run `tests/run-tests.sh` plus the lab `notifications-reply` scenario and record the results.

## 5. Documentation and rollout

- [x] 5.1 Document the inline-reply degradation (notification is shown without reply; the bridge stays alive) in the module comments for the presenter and `shell/services/Notifs.qml`.
- [x] 5.2 Note the behavior change in the change's proposal/design if any of the open questions are resolved during implementation.
- [ ] 5.3 After merge: bump the `caelestia-kde` input pin in `~/.config/home-manager`, run `home-manager switch`, restart `plasma-plasmashell` once, and restart `caelestia-activities.service`.
- [ ] 5.4 Verify live: `Snapshot` reports `supported: true` with no `error`; entering the Caelestia Activity yields `state: caelestia` and an error-free journal; a Slack/Beeper notification no longer disables the bridge.
- [ ] 5.5 Confirm the Plasma DND setting that makes `inhibited: true` (until 2027-01-20) is intentional; document it as unrelated to this fix.
- [ ] 5.6 Correct the stale `("Default", pre-existing)` label in `~/.config/home-manager/caelestia.nix` to the verified name `Another`, keep UUID `58866ae6-d479-4752-9546-0af404bd48d0`, and state that Activity display names are not used for resolution.
- [x] 5.7 Re-verify the mapping before rollout: `kactivitymanagerdrc` shows baseline `58866ae6…` and target `79a4e569…` (`Caelestia`); containments 415/416 belong to baseline, 479/480 to Caelestia and each carry `org.caelestia.notification-presenter`; panels 485/511 have an empty `activityId` (global), so the Panel Colorizer hiding path is still required.
