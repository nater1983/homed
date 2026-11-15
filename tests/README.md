# Tests

The test suite is split into static validation and isolated runtime integration
tests.

Run the complete suite with:

```sh
meson setup build -Dtests=true
ninja -C build
meson test -C build --print-errorlogs
```

## Static validation

- `dbus-xml-valid` validates the installed D-Bus introspection, bus policy,
  and PolicyKit XML and checks the expected Manager and Home methods.

## Runtime integration tests

- `varlink-integration` starts the Varlink listener on a temporary Unix socket.
  It covers NSS user, group, and membership lookups, interface introspection,
  request validation, and persistent and concurrent connections.
- `varlink-limits` verifies independent per-UID connection accounting.
- `dbus-integration` starts a private D-Bus daemon and registers the real
  `RclHome` implementation on that bus. It tests Manager lookups by name,
  UID, and PID, home lookup by name and UID, `ListHomes`, Home properties,
  and missing-user errors.
- The D-Bus integration executable also starts the real Varlink listener on a
  separate temporary socket. Its mixed concurrency test issues D-Bus and
  Varlink requests from concurrent threads against the same
  `rcl-nss.c` mutex.

The runtime tests use the local NSS database and expect the standard `root`
user and group entries. They do not require the production system bus, the
production `/run/systemd/userdb/io.systemd.Home` socket, systemd, or elogind.
The Varlink test-only startup API accepts a private socket path so the test
suite cannot replace or unlink the installed service socket.

## Installed runtime on Slackware

With the package installed and the SysVinit system bus running, check
activation from a regular user shell. Use a test host with no running
`homed` instance. `NameHasOwner` should return `false` before the lookup and
`true` afterward. The lookup should return `sbo` with a JSON record for
`root`, `true` for `incomplete`, and the object path of the Home record.

```sh
busctl --system call org.freedesktop.DBus /org/freedesktop/DBus \
  org.freedesktop.DBus NameHasOwner s org.freedesktop.home1
busctl --system call org.freedesktop.home1 /org/freedesktop/home1 \
  org.freedesktop.home1.Manager GetUserRecordByName s root
busctl --system call org.freedesktop.DBus /org/freedesktop/DBus \
  org.freedesktop.DBus NameHasOwner s org.freedesktop.home1
```

Read both properties on the Home path returned by the lookup. If upstream
`homectl` is installed, `homectl inspect root` also exercises the `sbo` reply.

```sh
busctl --system get-property org.freedesktop.home1 /org/freedesktop/home1/home/_0 \
  org.freedesktop.home1.Home UnixRecord
busctl --system get-property org.freedesktop.home1 /org/freedesktop/home1/home/_0 \
  org.freedesktop.home1.Home UserRecord
```

From the same regular user shell, verify elogind discovery and query homed
directly. The queries select `io.systemd.Home` and disable both local NSS
fallback and synthesized root records.

```sh
userdbctl services
userdbctl --service=io.systemd.Home --with-nss=no --synthesize=no user root
userdbctl --service=io.systemd.Home --with-nss=no --synthesize=no group root
```

On the test host, use a root shell to stop the current D-Bus owner and check
that it removes the Varlink socket. The script waits up to five seconds
without sending another request to homed, which would activate it again.

```sh
(
  set -eu
  socket=/run/systemd/userdb/io.systemd.Home
  test -S "$socket"
  test "$(stat -c '%a %u' "$socket")" = '666 0'
  stat -c '%a %U:%G %n' "$socket"
  pid_reply=$(busctl --system call org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus GetConnectionUnixProcessID s org.freedesktop.home1)
  pid=${pid_reply#u }
  test "$pid" -gt 1
  test "$(readlink -f "/proc/$pid/exe")" = /usr/libexec/homed
  kill -TERM "$pid"
  for attempt in 1 2 3 4 5; do
    if ! kill -0 "$pid" 2>/dev/null; then
      break
    fi
    sleep 1
  done
  if kill -0 "$pid" 2>/dev/null; then
    echo "homed did not exit after SIGTERM" >&2
    exit 1
  fi
  test ! -e "$socket"
  echo "homed exited and removed the Varlink socket"
)
```

Repeat the unprivileged activation and userdb checks to verify that the
listener is recreated and works after restart. Confirm the recreated socket
is owned by root and has mode `0666`.

```sh
stat -c '%a %U:%G %n' /run/systemd/userdb/io.systemd.Home
```
