#!/bin/bash
# Prints the SimpleMtkWlan in-driver log buffer (published as an ioreg property).
ioreg -a -r -c IOPCIDevice -l | python3 -I -c 'import plistlib,sys
def w(o):
    if isinstance(o, dict):
        if "SimpleMtkWlanLog" in o: print(o["SimpleMtkWlanLog"])
        for v in o.values(): w(v)
    elif isinstance(o, list):
        for v in o: w(v)
w(plistlib.loads(sys.stdin.buffer.read()))'
