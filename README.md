# homed

A Slackware-oriented, non-systemd implementation of a limited `org.freedesktop.home1` D-Bus compatibility service, following the build and packaging conventions of [`nater1983/hostnamed`](https://github.com/nater1983/hostnamed) `main`.

## Scope and safety

This initial release is a **read-only compatibility layer**, not a replacement for `systemd-homed`. It provides the system bus name `org.freedesktop.home1` at `/org/freedesktop/home1` and implements `GetHomeByName`, `GetHomeByUID`, `ListHomes`, and the empty `AutoLogin` property from local NSS/passwd records. Accounts are reported as `inactive`. It does not create, remove, encrypt, resize, lock, or activate managed home areas. Do not use it as an encrypted-home backend or wire it into login activation expecting those operations to work.

Polkit policy and rules are installed in the same manner as the hostnamed project; management actions are reserved for future implementation and are not exposed by this version. No systemd units, systemd libraries, or systemd init scripts are installed.

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
busctl --system call org.freedesktop.home1 /org/freedesktop/home1 org.freedesktop.home1.Manager ListHomes
```

## Status

This is a first packaging baseline. Full `systemd-homed` compatibility requires implementing and testing the per-home object API, Create/Register/Activate/Deactivate/Remove operations, user-record parsing, storage backends, and authorization before it can be considered a functional home manager.
