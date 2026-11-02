#!/usr/bin/env python3
import sys
import xml.etree.ElementTree as ET

for path in sys.argv[1:]:
    root = ET.parse(path).getroot()
    if root.tag not in ("node", "busconfig", "policyconfig"):
        raise SystemExit(f"unexpected XML root {root.tag!r} in {path}")

    if root.tag == "busconfig":
        default = root.find("policy[@context='default']")
        if default is None:
            raise SystemExit("default D-Bus policy missing")
        property_rules = [
            rule for rule in default.findall("allow")
            if rule.get("send_interface") == "org.freedesktop.DBus.Properties"
        ]
        property_members = {rule.get("send_member") for rule in property_rules}
        if property_members != {"Get", "GetAll"}:
            raise SystemExit(
                f"default D-Bus property access is {property_members!r}, "
                "expected only Get and GetAll"
            )
        if any(rule.get("send_interface") == "org.freedesktop.home1.Home"
               for rule in default.findall("allow")):
            raise SystemExit("default policy must not allow Home lifecycle methods")
        for method in ("GetHomeByName", "GetHomeByUID",
                       "GetUserRecordByUID", "GetUserRecordByName",
                       "GetUserRecordByPID", "ListHomes"):
            if not any(rule.get("send_interface") == "org.freedesktop.home1.Manager"
                       and rule.get("send_member") == method
                       for rule in default.findall("allow")):
                raise SystemExit(f"default policy does not allow Manager.{method}")

    if root.tag == "node":
        interfaces = {i.get("name"): i for i in root.findall("interface")}
        manager = interfaces.get("org.freedesktop.home1.Manager")
        home = interfaces.get("org.freedesktop.home1.Home")
        if manager is None or home is None:
            raise SystemExit("manager or home interface missing")
        required = {
            "GetHomeByName": ("u", "s", "u", "s", "s", "s", "o"),
            "GetHomeByUID": ("s", "s", "u", "s", "s", "s", "o"),
            "GetUserRecordByUID": ("a{sv}", "b"),
            "GetUserRecordByName": ("a{sv}", "b"),
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
    print(f"valid XML: {path}")
