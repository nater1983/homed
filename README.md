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

Each NSS account discovered by the daemon is represented by a per-home object at `/org/freedesktop/home1/home/_UID`. These objects expose read-only account properties.

The `State` property reports conventional home-directory availability: it returns `active` when the NSS home path exists, is a directory, and is readable/searchable by the daemon; otherwise it returns `inactive`. The manager methods `GetHomeByName`, `GetHomeByUID`, and `ListHomes` use the same check so their state values agree with the per-home property. This is an availability indication only; it does not mean that `homed` has unlocked, mounted, or activated managed storage.

The `Activate`, `Deactivate`, `Lock`, `Unlock`, and `Resize` methods are declared but return `org.freedesktop.home1.NotSupported`.

The system-bus policy reserves ownership of `org.freedesktop.home1` for root and allows other callers to use the read-only Manager methods, read properties, and use standard introspection and peer methods. The installed Polkit actions and group rule are reserved for future managed-home operations. The daemon currently makes no Polkit authorization calls.

User records contain the NSS-derived fields `userName`, `uid`, `gid`, `realName`, `homeDirectory`, `shell`, `service`, and `disposition`. D-Bus records identify their source as `nss`.

In 0.4.0, `GetUserRecordByName` and `GetUserRecordByUID` return the upstream `sbo` signature: a JSON user record, the `incomplete` flag, and a Home object path. Clients using the previous `a{sv}b` reply must decode the JSON string. The homed-specific `GetUserRecordByPID` still returns `a{sv}b`. Home objects also provide the read-only `UnixRecord` and `UserRecord` properties.

An empty name in `GetHomeByName` or `GetUserRecordByName` looks up the calling user's NSS account.

### Varlink interface

The Varlink socket is:

```
/run/systemd/userdb/io.systemd.Home
```

The service implements:

- `GetUserRecord`
- `GetGroupRecord`
- `GetMemberships`

All three methods require `"service":"io.systemd.Home"`. Unsupported lookup
filters return `InvalidParameter` with the parameter name. Use
`org.varlink.service.GetInterfaceDescription` to see the supported parameters.

The interface returns NSS records with `incomplete=false` because there are no privileged fields to redact. The Varlink provider identifies itself as `io.systemd.Home`. For membership queries with only a user name or group name, send the Varlink `more` flag to receive all matching pairs.

The socket path is used for user-database discovery compatibility; it does not mean that systemd is required or installed.

The listener uses a mode-`0666` socket for local account lookups. The socket directory must be owned by root or the daemon user and writable only by its owner. Admission uses the peer UID reported by the kernel for connection accounting.

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
| `homectl` | `false` | Build and install the read-only client and its manual page. |
| `privileged-group` | `wheel` | Group used by the installed Polkit rules. |
| `varlink-vendor` | `N4t3R` | Vendor name reported by `org.varlink.service.GetInfo`. |
| `dbus_system_service_dir` | empty | Override the D-Bus system service directory. |
| `dbus_system_conf_dir` | empty | Override the D-Bus system policy directory. |
| `polkit_policy_dir` | empty | Override the Polkit action policy directory. |

When an override directory is empty, Meson uses the corresponding directory reported by the installed dependency where available, or the project default.

## Read-only homectl client

Enable the client with `-Dhomectl=true` in Meson or run the SlackBuild with
`HOMECTL=true`. It installs `homectl` in the configured `bindir` and
includes `homectl(1)`. The default build leaves the command name available
for an existing upstream client.

```sh
homectl list
homectl inspect nathan
homectl inspect 1000
homectl --json inspect root
homectl --json list
```

The client reads accounts over the system D-Bus and can run as an ordinary
user. Use Slackware's account tools to create or modify accounts.

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

varlinkctl --more call unix:/run/systemd/userdb/io.systemd.Home \
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
