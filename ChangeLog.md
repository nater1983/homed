# ChangeLog

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
