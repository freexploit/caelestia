# activity-coordinator

## Purpose

Define the serialized owner of the Activity-scoped shell: preflight classification, entry and exit journaling, retry and recovery behavior, Activity identity resolution, and the meaning of each `activities.json` field. The coordinator distinguishes backend capability failures from individual notification outcomes so a transient presenter problem cannot silently block the managed shell.

## Requirements

### Requirement: Preflight distinguishes capability failures from notification outcomes

`preflight()` MUST treat a presenter `Snapshot().supported == false` as a blocking capability failure. It MUST NOT block entry for conditions that are scoped to an individual notification or history entry.

#### Scenario: Per-notification decline does not block entry

- **WHEN** the presenter reports `supported: true` while one notification has been declined or degraded
- **THEN** `preflight()` passes and the managed shell is entered

#### Scenario: Presenter capability failure blocks entry with its reason

- **WHEN** the presenter reports `supported: false` with a capability error
- **THEN** `preflight()` raises with that error text and no managed surface is borrowed

### Requirement: Entry is retried after a transient presenter failure

The coordinator MUST retry `enter()` after a failed `preflight()` while the current Activity still matches the target, with bounded backoff and without requiring an Activity change. It MUST stop retrying only when the failure is a persistent capability failure or a non-preflight entry failure, and it MUST surface that state in the journal.

#### Scenario: Presenter recovers without an Activity change

- **WHEN** `enter()` fails because the presenter is temporarily unsupported and the presenter becomes supported again while the target Activity is current
- **THEN** the coordinator retries `enter()` and completes it without any Activity switch

#### Scenario: Persistent failure stops the retry loop

- **WHEN** `enter()` fails for a persistent reason (for example, the baseline Activity is missing or the Activity manager rejects the switch)
- **THEN** the coordinator stops retrying until the Activity or the service state changes
- **AND** the journal records the failure

#### Scenario: Two failing attempts do not duplicate borrowed state

- **WHEN** `enter()` fails after partially borrowing surfaces and is retried
- **THEN** `leave()` has restored every recorded surface before the retry starts
- **AND** the journal's panel and tracker ownership records remain accurate

### Requirement: The journal error field describes the current attempt

`activities.json` `error` MUST be cleared when a new entry attempt begins and when `enter()` succeeds. It MUST contain only the most recent failure reason, so the file never reports a failure that a later success has superseded.

#### Scenario: Successful entry clears a prior error

- **WHEN** a previous attempt left `error` set and a later `enter()` succeeds
- **THEN** the journal has `state: caelestia` and no `error` key, while all other fields retain their documented meaning

#### Scenario: New attempt clears the previous failure

- **WHEN** the coordinator begins `enter()` after a recorded failure
- **THEN** the journal is written with `state: entering` and without the old `error`

#### Scenario: Failure is recorded with the failing state

- **WHEN** `enter()` fails and `leave()` completes
- **THEN** the journal has the recovered `state` (or `recovering` if recovery was incomplete) and `error` contains the failure reason

### Requirement: Activities are identified by UUID, not display name

The baseline Activity MUST be referenced by UUID. The target Activity MUST be resolved from the persisted identity file or an exact name match only when no identity exists. Renaming an Activity in Plasma MUST NOT change coordinator behavior.

#### Scenario: Renaming the baseline Activity is inert

- **WHEN** the baseline Activity is renamed in Plasma while its UUID is unchanged
- **THEN** the coordinator still treats that UUID as baseline and the target Activity still starts the managed shell

#### Scenario: Renaming the target Activity with an identity file is inert

- **WHEN** `activity-identity.json` names the configured target and the Activity is renamed in Plasma
- **THEN** the coordinator still resolves the target from the persisted UUID
- **AND** it does not create a second Activity or fail on the name mismatch

### Requirement: Journal schema stays version 1 and backward compatible

`activities.json` MUST keep `version: 1` and its existing fields (`state`, `panels`, `meta`, `trackerOwned`, optional `error`). Owners MUST tolerate an absent `error` key and a `baseline` state that previously carried a stale error.

#### Scenario: Existing journal is accepted

- **WHEN** the coordinator starts with an existing version-1 journal that has a stale `error` and `state: baseline`
- **THEN** it loads the journal, clears the stale error on the next attempt, and performs no migration
