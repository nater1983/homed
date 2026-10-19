# homed

A Slackware-oriented, non-systemd implementation of a limited `org.freedesktop.home1` D-Bus compatibility service, following the build and packaging conventions of [`nater1983/hostnamed`](https://github.com/nater1983/hostnamed).

## Scope and safety

This release is an **NSS-backed compatibility layer**, not a complete replacement for `systemd-homed`. It provides the system bus name `org.freedesktop.home1` at `/org/freedesktop/home1` and a Varlink listener for the `io.systemd.UserDatabase` interface.

The D-Bus manager implements:
- `GetHomeByName`, `GetHomeByUID`, and `ListHomes`
- `GetUserRecordByName`, `GetUserRecordByUID`, and `GetUserRecordByPID`
- The read-only `AutoLogin` property (currently empty)

The Varlink socket is `/run/systemd/userdb/io.systemd.Home`. It supports `GetUserRecord`, `GetGroupRecord`, and `GetMemberships` for NSS accounts and groups. Records derived from NSS return `incomplete=true`, because NSS does not provide managed-home storage, binding, signature, or secret metadata.

Each discovered NSS account has a per-home object at `/org/freedesktop/home1/home/_UID` exposing read-only account properties. The object declares `Activate`, `Deactivate`, `Lock`, `Unlock`, and `Resize`, but these operations return `org.freedesktop.home1.NotSupported` because no managed storage backend exists.

The service does not create, remove, encrypt, resize, lock, or activate managed home areas. Do not use it as an encrypted-home backend or wire it into login activation expecting those operations to work. No systemd units, systemd libraries, or systemd init scripts are installed; the socket path is used for user-database discovery compatibility.

## Build

Dependencies: Meson >= 0.58, Ninja, C compiler, GLib/GIO >= 2.56, JSON-GLib >= 1.4, D-Bus development metadata, and Polkit >= 0.113.

```sh
meson setup build --prefix=/usr --libexecdir=/usr/libexec --sysconfdir=/etc -Dprivileged-group=wheel
ninja -C build
meson test -C build --print-errorlogs
DESTDIR="$PWD/stage" ninja -C build install
```

## Test

D-Bus:

```sh
busctl --system introspect org.freedesktop.home1 /org/freedesktop/home1
busctl --system call org.freedesktop.home1 /org/freedesktop/home1 org.freedesktop.home1.Manager GetHomeByName s root
busctl --system call org.freedesktop.home1 /org/freedesktop/home1 org.freedesktop.home1.Manager GetUserRecordByName s root
busctl --system call org.freedesktop.home1 /org/freedesktop/home1 org.freedesktop.home1.Manager GetUserRecordByPID u 1
busctl --system call org.freedesktop.home1 /org/freedesktop/home1 org.freedesktop.home1.Manager ListHomes
```

Varlink (with a Varlink client installed):

```sh
varlink call unix:/run/systemd/userdb/io.systemd.Home io.systemd.UserDatabase.GetUserRecord '{"userName":"root","service":"io.systemd.Home"}'
varlink call unix:/run/systemd/userdb/io.systemd.Home io.systemd.UserDatabase.GetGroupRecord '{"groupName":"root","service":"io.systemd.Home"}'
varlink call unix:/run/systemd/userdb/io.systemd.Home io.systemd.UserDatabase.GetMemberships '{"userName":"root","service":"io.systemd.Home"}'
```

## Status

The D-Bus and Varlink interfaces expose NSS-backed lookup and intentionally incomplete records. Full compatibility still requires accurate managed-home record semantics, Create/Register/Activate/Deactivate/Remove operations, storage backends, signals tied to actual changes, and authorization before this can be considered a functional home manager.
