# homed

A Slackware-oriented, non-systemd implementation of a limited `org.freedesktop.home1` D-Bus compatibility service and the `io.systemd.UserDatabase` Varlink interface.

`homed` provides NSS-backed account and group lookups for systems that do not run systemd. It is designed to integrate with the existing D-Bus, Polkit, and Name Service Switch infrastructure rather than replace the system's account or home-directory management tools.

## Current status

**This is an NSS-backed compatibility service, not a complete replacement for `systemd-homed`.**

The daemon exposes:

- The system-bus name `org.freedesktop.home1`.
- The manager object at `/org/freedesktop/home1`.
- A Varlink listener at `/run/systemd/userdb/io.systemd.Home` for `io.systemd.UserDatabase`.

The service reads account information from NSS. It does not create or manage home storage, encrypt home directories, or activate login sessions.

### D-Bus interface

The manager currently implements:

| Method / property | Description |
| --- | --- |
| `GetHomeByName` | Look up an NSS account by user name. |
| `GetHomeByUID` | Look up an NSS account by numeric UID. |
| `ListHomes` | Enumerate accounts discovered through NSS. |
| `GetUserRecordByName` | Return a user record by user name. |
| `GetUserRecordByUID` | Return a user record by numeric UID. |
| `GetUserRecordByPID` | Resolve the real UID of a process and return its user record. |
| `AutoLogin` | Read-only property; currently returns an empty array. |

Each NSS account discovered by the daemon is represented by a per-home object at `/org/freedesktop/home1/home/_UID`. These objects expose read-only account properties. The `Activate`, `Deactivate`, `Lock`, `Unlock`, and `Resize` methods are declared but return `org.freedesktop.home1.NotSupported`.

User records currently contain the NSS-derived fields `userName`, `uid`, `gid`, `realName`, `homeDirectory`, `shell`, `service`, and `disposition`. D-Bus records identify their source as `nss`.

### Varlink interface

The Varlink socket is:

```
/run/systemd/userdb/io.systemd.Home
```

The service implements:

- `GetUserRecord`
- `GetGroupRecord`
- `GetMemberships`

The interface returns records for accounts and groups available through NSS. NSS does not provide the managed-home metadata, secrets, bindings, or signatures required for a complete systemd-homed record, so NSS-derived user and group records are returned with `incomplete=true`. The Varlink provider identifies itself as `io.systemd.Home`.

The socket path is used for user-database discovery compatibility; it does not mean that systemd is required or installed.

### NSS concurrency

D-Bus method handling and Varlink request handling can run concurrently. Both interfaces use a shared NSS mutex to serialize access to NSS account and group lookups, avoiding concurrent use of NSS state by the two service paths. The test suite includes a mixed D-Bus/Varlink concurrency regression test.

## Limitations

The following functionality is not implemented:

- Creating, registering, or removing managed home areas.
- Encrypted home images or other managed storage backends.
- Home activation and deactivation.
- Locking, unlocking, or resizing managed homes.
- Complete systemd-homed user-record parsing, validation, and metadata.
- Lifecycle signals tied to actual managed-home state changes.

Do not use this service as an encrypted-home backend or rely on it for home activation during login. The declared per-home lifecycle methods are compatibility placeholders and return `NotSupported`.

## Dependencies

Build dependencies:

- Meson >= 0.58
- Ninja
- A C compiler
- GLib and GIO >= 2.56
- JSON-GLib >= 1.4
- Polkit >= 0.113
- D-Bus development metadata (used to locate the system-bus service directory when available)

The daemon does not link against systemd or require systemd init units, libraries, or service management. It uses the system D-Bus and Polkit installations.

## Build

A typical Slackware build:

```sh
meson setup build \
  --prefix=/usr \
  --libexecdir=/usr/libexec \
  --sysconfdir=/etc \
  -Dprivileged-group=wheel

ninja -C build
```

Run the test suite:

```sh
meson test -C build --print-errorlogs
```

Install into the system or a package staging directory:

```sh
ninja -C build install
DESTDIR="$PWD/package" ninja -C build install
```

Tests are enabled by default. To configure without building the test suite, pass `-Dtests=false` to `meson setup`.

### Meson options

| Option | Default | Purpose |
| --- | --- | --- |
| `tests` | `true` | Build and register the test suite. |
| `privileged-group` | `wheel` | Group used by the installed Polkit rules. |
| `dbus_system_service_dir` | empty | Override the D-Bus system service directory. |
| `dbus_system_conf_dir` | empty | Override the D-Bus system policy directory. |
| `polkit_policy_dir` | empty | Override the Polkit action policy directory. |

When an override directory is empty, Meson uses the corresponding directory reported by the installed dependency where available, or the project default.

## Testing the installed service

### D-Bus

Inspect the manager interface:

```sh
busctl --system introspect org.freedesktop.home1 /org/freedesktop/home1
```

Look up the root account:

```sh
busctl --system call org.freedesktop.home1 \
  /org/freedesktop/home1 \
  org.freedesktop.home1.Manager GetUserRecordByName s root
```

Look up a process by PID:

```sh
busctl --system call org.freedesktop.home1 \
  /org/freedesktop/home1 \
  org.freedesktop.home1.Manager GetUserRecordByPID u 1
```

Other manager methods can be exercised with `GetHomeByName`, `GetHomeByUID`, `GetUserRecordByUID`, and `ListHomes`.

### Varlink

Use `varlinkctl` to query the root user, root group, and root memberships:

```sh
varlinkctl call unix:/run/systemd/userdb/io.systemd.Home \
  io.systemd.UserDatabase.GetUserRecord \
  '{"userName":"root","service":"io.systemd.Home"}'

varlinkctl call unix:/run/systemd/userdb/io.systemd.Home \
  io.systemd.UserDatabase.GetGroupRecord \
  '{"groupName":"root","service":"io.systemd.Home"}'

varlinkctl call unix:/run/systemd/userdb/io.systemd.Home \
  io.systemd.UserDatabase.GetMemberships \
  '{"userName":"root","service":"io.systemd.Home"}'
```

## Installed components

The installation includes:

- The `homed` daemon under Meson's `libexecdir` (typically `/usr/libexec/homed`).
- The D-Bus system service activation file.
- The D-Bus system policy and interface description.
- The Polkit action policy and rules.

The Varlink socket is created at runtime under `/run/systemd/userdb/`.

## Logging

The daemon reports startup and service-registration activity through syslog using the `homed` identifier. The startup message uses the package version configured by Meson.

## License

GPL-2.0-or-later.
