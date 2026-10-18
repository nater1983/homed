#!/usr/bin/env python3
import sys
import xml.etree.ElementTree as ET
for path in sys.argv[1:]:
    ET.parse(path)
    print(f"valid XML: {path}")
