# ChangeLog

## 0.4.0 — Development (Phase 7)

- Add Varlink regression tests for oversized request frames and EOF on incomplete frames, checking protocol error/connection closure and preventing partial request dispatch.

- Return Varlink `InvalidParameter` for empty frames while preserving the connection; return a protocol error and close on oversized frames (1 MiB limit).
- Add an empty-frame recovery regression test; oversized-frame and incomplete-frame regression coverage is still pending.

- Reject Varlink requests with non-boolean `more` fields using `org.varlink.service.InvalidParameter` instead of silently ignoring the invalid value.
- Add Varlink integration coverage for string, numeric, and null `more` values, and verify the connection remains usable after validation errors.


## 0.4.beta — 2026-11-15

Beta prerelease following the 0.4.alpha development baseline. Slackware validation: 3/3 Meson suites and 33/33 individual tests passed. This is a prerelease; protocol hardening, security review, runtime integration, packaging review, and final release verification remain planned for 0.4.0.

- Fixed NSS supplementary-group lookup failures being mistaken for missing memberships across user-only, combined user/group, and group-only Varlink filters; report `ServiceNotAvailable` on failure.
- Added test-only supplementary-group failure injection, regression tests for all three filter modes, and a recovery check.
- Bounded `getgrouplist()` retries and group-count allocation, and removed duplicate supplementary group IDs from successful results.
- Snapshotted resolved passwd identity before subsequent NSS group lookups, with a combined-filter regression test.
- Added configurable `varlink-vendor` Meson option for Varlink service metadata.

## 0.4.alpha — Development baseline (date not verified)

Initial 0.4 development work, prior to the beta NSS error-handling fixes:

- Reported NSS home-directory availability through per-home `State`, `GetHomeByName`, `GetHomeByUID`, and `ListHomes`, without claiming managed-home activation; added consistency tests.
- Added isolated D-Bus integration tests for account and home lookups, enumeration, missing users, read-only properties, and unsupported lifecycle operations.
- Added isolated Varlink integration tests for user, group, and membership records, invalid requests, unsupported methods, service-name validation, persistent connections, and concurrent clients.
- Added mixed D-Bus/Varlink NSS concurrency coverage and private-path Varlink listener tests to avoid touching the production socket.
- Allowed unprivileged system-bus callers to use read-only `GetUserRecordByUID`, `GetUserRecordByName`, and `GetUserRecordByPID`.
- Kept Varlink connections open across multiple NUL-terminated requests, with pipelined-request tests.
- Unified Varlink supplementary-group membership results across user and group queries; added membership consistency tests.
- Added Varlink listener restart tests, bounded NSS mutex acquisition for D-Bus and Varlink requests, lock-contention tests, and simulated NSS provider re-entry coverage.

## 0.3.1

- Split startup and Varlink listener registration into separate syslog messages.

## 0.3.0

- Added a Varlink listener at `/run/systemd/userdb/io.systemd.Home` for the `io.systemd.UserDatabase` interface.
- Added NSS-backed `GetUserRecord`, `GetGroupRecord`, and `GetMemberships` lookups.
- Marked NSS-derived user and group records as incomplete because they do not contain managed-home metadata.
- Added Varlink request validation, NUL-terminated message handling, and errors for unsupported methods and lookup failures.
- Added JSON-GLib as a build dependency for Varlink message parsing and serialization.
- Updated the README and manual page with Varlink usage, build dependencies, and current compatibility limitations.
- Improved Varlink source formatting and corrected name/ID lookup validation and request framing.

## 0.2.0

- Corrected GetHomeByName and GetHomeByUID D-Bus output signatures and value order.
- Added GetUserRecordByPID alongside UID and name lookups.
- NSS-derived user records now report incomplete=true instead of claiming to be complete managed-home records.
- Added per-home D-Bus objects with read-only account properties and explicit unsupported lifecycle operations.
- Added per-home Changed and Removed signal declarations; lifecycle/storage mutations are not implemented.

## 0.1.0

- Initial non-systemd D-Bus compatibility baseline.
- Meson layout and Slackware packaging conventions modeled on nater1983/hostnamed main.
- Read-only NSS-backed home lookup methods.
