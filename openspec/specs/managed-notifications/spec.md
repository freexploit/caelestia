# managed-notifications

## Purpose

Define whether the scoped Plasma notification bridge is usable, how per-notification declines and degraded deliveries are reported to their caller, and how the shell watcher survives them. Presenter support reflects backend capability and lifecycle conditions only; notification content never latches the presenter off.

## Requirements

### Requirement: Presenter support reflects backend capability only

`Snapshot().supported` and `Acquire()` SHALL be gated only by capability and lifecycle conditions: missing popup instantiator/model/history, a non-authoritative process owner, duplicate registration, and lease state. Notification content, action metadata, and image encodability MUST NOT affect presenter support.

#### Scenario: Reply-capable notification does not disable the presenter

- **WHEN** a leased shell calls `Describe(id)` for a notification whose model row has `HasReplyActionRole` set
- **THEN** the call returns a describable map without a reply affordance
- **AND** `Snapshot().supported` remains `true` and a subsequent `Acquire()` succeeds

#### Scenario: Malformed action metadata does not disable the presenter

- **WHEN** `Describe(id)` encounters a notification whose action name and label lists differ in length
- **THEN** the notification is delivered without sender actions
- **AND** `Snapshot().supported` remains `true`

#### Scenario: Unencodable image does not disable the presenter

- **WHEN** `Describe(id)` cannot encode the notification image as PNG
- **THEN** the notification is delivered without an image
- **AND** `Snapshot().supported` remains `true`

#### Scenario: Capability failure still blocks the presenter

- **WHEN** the presenter is not inside the authoritative Plasma notification process, or the popup instantiator/model is unavailable
- **THEN** `Snapshot().supported` is `false` and `Snapshot().error` names that capability failure

### Requirement: Per-notification outcomes are local and reported to the caller

`Describe(id)` and `Existing()` MUST report a per-notification outcome (delivered with degradation, or declined) through the returned value or a per-call diagnostic. They MUST NOT change presenter-wide error state, MUST NOT emit `PolicyChanged` for a notification-content condition, and MUST NOT release an active lease.

#### Scenario: Decline is distinguishable from an absent notification

- **WHEN** a notification cannot be described at all (for example, the model row expired between notification and query)
- **THEN** the caller receives an explicit decline signal for that id
- **AND** `Snapshot().error` and `Snapshot().supported` are unchanged

#### Scenario: Replay skips undeclarable history entries

- **WHEN** `Existing()` encounters a history entry with a per-notification decline condition
- **THEN** that entry is omitted from the returned list
- **AND** every other entry is returned and the presenter remains supported

### Requirement: The shell notification bridge survives per-notification outcomes

The shell watcher MUST keep its D-Bus registration, Plasma watcher registration, and presenter lease across per-notification declines and degraded deliveries. It MUST NOT call the fatal path (clearing readiness, unregistering, releasing the lease) for a notification-content condition.

#### Scenario: Declined notification leaves the bridge intact

- **WHEN** a notification arrives while the shell is leased and its description is declined
- **THEN** the watcher remains registered, `ready` stays `true`, and `active` stays `true`
- **AND** the decline is logged once with the notification id and condition

#### Scenario: Later notifications still arrive after a decline

- **WHEN** a decline has occurred and another notification arrives
- **THEN** the later notification is delivered to the shell normally

#### Scenario: Presenter capability loss still tears down the bridge

- **WHEN** the presenter stops being supported, or its owner changes
- **THEN** the shell watcher stops and reports the capability error as before
