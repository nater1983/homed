#!/usr/bin/env python3
import sys
import xml.etree.ElementTree as ET

for path in sys.argv[1:]:
    root = ET.parse(path).getroot()
    if root.tag not in ("node", "busconfig", "policyconfig"):
        raise SystemExit(f"unexpected XML root {root.tag!r} in {path}")

    if root.tag == "busconfig":
        destination = "org.freedesktop.home1"
        owner = root.find("policy[@user='root']")
        default = root.find("policy[@context='default']")
        if owner is None or default is None or len(root) != 2:
            raise SystemExit("expected only root and default D-Bus policies")
        expected_owner = [
            {"own": destination},
            {"send_destination": destination},
            {"receive_sender": destination},
        ]
        expected_default = [{"receive_sender": destination}]
        read_only = {
            "org.freedesktop.DBus.Introspectable": ("Introspect",),
            "org.freedesktop.DBus.Peer": ("Ping", "GetMachineId"),
            "org.freedesktop.DBus.Properties": ("Get", "GetAll"),
            "org.freedesktop.home1.Manager": (
                "GetHomeByName", "GetHomeByUID", "GetUserRecordByUID",
                "GetUserRecordByName", "GetUserRecordByPID", "ListHomes",
            ),
        }
        for interface, members in read_only.items():
            for member in members:
                expected_default.append({"send_destination": destination,
                                         "send_interface": interface,
                                         "send_member": member})
        for policy, expected in ((owner, expected_owner),
                                 (default, expected_default)):
            actual = [rule.attrib for rule in policy.findall("allow")]
            if len(policy) != len(expected) or any(rule not in actual for rule in expected):
                raise SystemExit(f"unexpected D-Bus rules for {policy.attrib}: {actual!r}")

    if root.tag == "policyconfig":
        actions = root.findall("action")
        expected_ids = {"org.freedesktop.home1.manage-home",
                        "org.freedesktop.home1.activate-home"}
        if len(actions) != len(expected_ids) or {a.get("id") for a in actions} != expected_ids:
            raise SystemExit("unexpected homed Polkit actions")
        for action in actions:
            defaults = action.find("defaults")
            expected = {"allow_any": "auth_admin", "allow_inactive": "auth_admin",
                        "allow_active": "auth_admin_keep"}
            if defaults is None or len(defaults) != len(expected) or {
                rule.tag: rule.text for rule in defaults
            } != expected:
                raise SystemExit(f"unexpected Polkit defaults for {action.get('id')}")

    if root.tag == "node":
        interfaces = {i.get("name"): i for i in root.findall("interface")}
        manager = interfaces.get("org.freedesktop.home1.Manager")
        home = interfaces.get("org.freedesktop.home1.Home")
        if manager is None or home is None:
            raise SystemExit("manager or home interface missing")
        required = {
            "GetHomeByName": ("u", "s", "u", "s", "s", "s", "o"),
            "GetHomeByUID": ("s", "s", "u", "s", "s", "s", "o"),
            "GetUserRecordByUID": ("s", "b", "o"),
            "GetUserRecordByName": ("s", "b", "o"),
            "GetUserRecordByPID": ("a{sv}", "b"),
            "ListHomes": ("a(susussso)",),
        }
        for name, expected_out in required.items():
            method = manager.find(f"method[@name='{name}']")
            if method is None:
                raise SystemExit(f"missing Manager.{name}")
            actual = tuple(arg.get("type") for arg in method.findall("arg")
                           if arg.get("direction") == "out")
            if actual != expected_out:
                raise SystemExit(f"{name} outputs {actual!r}, expected {expected_out!r}")
        for name in ("Activate", "Deactivate", "Lock", "Unlock", "Resize"):
            if home.find(f"method[@name='{name}']") is None:
                raise SystemExit(f"missing Home.{name}")
        for name in ("Changed", "Removed"):
            if home.find(f"signal[@name='{name}']") is None:
                raise SystemExit(f"missing Home.{name} signal")
        for name, signature in (("UnixRecord", "(suusss)"), ("UserRecord", "(sb)")):
            prop = home.find(f"property[@name='{name}']")
            if prop is None or prop.get("type") != signature or prop.get("access") != "read":
                raise SystemExit(f"expected read-only Home.{name} with type {signature}")
    print(f"valid XML: {path}")
