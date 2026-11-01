# Tests

The test suite is split into static validation and isolated runtime integration
tests.

Run the complete suite with:

\`\`\`sh
meson setup build -Dtests=true
ninja -C build
meson test -C build --print-errorlogs
\`\`\`

## Static validation

- \`dbus-xml-valid\` validates the installed D-Bus introspection, bus policy,
  and PolicyKit XML and checks the expected Manager and Home methods.

## Runtime integration tests

- \`varlink-integration\` starts the Varlink listener on a temporary Unix socket.
  It tests NSS user, group, and membership lookups; UID/name consistency;
  malformed requests; unknown methods; service validation; multiple requests
  on one connection; and concurrent clients.
- \`dbus-integration\` starts a private D-Bus daemon and registers the real
  \`RclHome\` implementation on that bus. It tests Manager lookups by name,
  UID, and PID; home lookup by name and UID; ListHomes; a per-home property;
  and missing-user errors.
- The D-Bus integration executable also starts the real Varlink listener on a
  separate temporary socket. Its mixed concurrency test issues D-Bus and
  Varlink requests from concurrent threads against the same
  \`rcl-nss.c\` mutex.

The runtime tests use the local NSS database and expect the standard \`root\`
user and group entries. They do not require the production system bus, the
production \`/run/systemd/userdb/io.systemd.Home\` socket, systemd, or elogind.
The Varlink test-only startup API accepts a private socket path so the test
suite cannot replace or unlink the installed service socket.

## Elogind userdb integration

The isolated tests verify the Varlink service protocol directly. To verify
elogind's userdb multiplexer on an installed Slackware system, start homed and
run:

\`\`\`sh
userdbctl services
userdbctl user root
userdbctl group root
\`\`\`

Confirm that \`io.systemd.Home\` is listed as an available userdb service and
that the returned user and group records match the direct Varlink responses.
This host-level check is intentionally not part of the default Meson suite:
it depends on a running installed homed service and the host's elogind setup.
