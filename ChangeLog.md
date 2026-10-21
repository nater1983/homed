# ChangeLog

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
