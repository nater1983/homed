# homed

A Slackware-oriented, non-systemd implementation of a limited `org.freedesktop.home1` D-Bus compatibility service, following the build and packaging conventions of [`nater1983/hostnamed`](https://github.com/nater1983/hostnamed).

## Scope and safety

This release is an **NSS-backed compatibility layer**, not a complete replacement for `systemd-homed`. It provides the system bus name `org.freedesktop.home1` at `/org/freedesktop/home1`.

Implemented manager calls:
- `GetHomeByName`, `GetHomeByUID`, and `ListHomes`
- `GetUserRecordByName`, `GetUserRecordByUID`, and `GetUserRecordByPID`
- The read-only `AutoLogin` property (currently empty)

User records are derived from NSS/passwd fields. They intentionally return `incomplete=true`, because NSS does not provide managed-home storage, binding, signature, or secret metadata. Each discovered NSS account has a per-home object at `/org/freedesktop/home1/home/_UID` exposing read-only account properties. The object declares `Activate`, `Deactivate`, `Lock`, `Unlock`, and `Resize`, but these operations return `org.freedesktop.home1.NotSupported` because no managed storage backend exists.

The per-home interface declares `Changed` and `Removed` signals for future state-changing operations; this version does not perform lifecycle mutations and therefore does not emit them. It does not create, remove, encrypt, resize, lock, or activate managed home areas. Do not use it as an encrypted-home backend or wire it into login activation expecting those operations to work.

Polkit policy files may be installed for future management work, but this compatibility layer does not yet implement home-management authorization or storage operations. No systemd units, systemd libraries, or systemd init scripts are installed.

## Build

Dependencies: Meson >= 0.58, Ninja, C compiler, GLib/GIO >= 2.56, D-Bus development metadata, and Polkit >= 0.113.

```sh
meson setup build --prefix=/usr --libexecdir=/usr/libexec --sysconfdir=/etc -Dprivileged-group=wheel
ninja -C build
meson test -C build --print-errorlogs
DESTDIR="$PWD/stage" ninja -C build install
```

## Test

```sh
busctl --system introspect org.freedesktop.home1 /org/freedesktop/home1
busctl --system call org.freedesktop.home1 /org/freedesktop/home1 org.freedesktop.home1.Manager GetHomeByName s root
busctl --system call org.freedesktop.home1 /org/freedesktop/home1 org.freedesktop.home1.Manager GetUserRecordByName s root
busctl --system call org.freedesktop.home1 /org/freedesktop/home1 org.freedesktop.home1.Manager GetUserRecordByPID u 1
busctl --system call org.freedesktop.home1 /org/freedesktop/home1 org.freedesktop.home1.Manager ListHomes
busctl --system introspect org.freedesktop.home1 /org/freedesktop/home1/home/_0
```

## Status

The current scope covers lookup, honest incomplete records, and per-home object introspection. Full compatibility still requires accurate systemd-homed record semantics, Create/Register/Activate/Deactivate/Remove operations, storage backends, signals tied to actual changes, and authorization before it can be considered a functional home manager.
