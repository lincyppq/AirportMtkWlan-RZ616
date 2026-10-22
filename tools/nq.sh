#!/bin/bash
# networkQuality on one interface, printed in Mbit/s. Usage: tools/nq.sh [iface]
networkQuality -I "${1:-en1}" -s -c 2>/dev/null | python3 -I -c 'import json,sys
d=json.load(sys.stdin); print("down %.1f  up %.1f Mbit/s" % (d["dl_throughput"]/1e6, d["ul_throughput"]/1e6))'
