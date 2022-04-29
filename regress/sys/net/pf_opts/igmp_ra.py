#!/usr/local/bin/python3

print("send internet group management protocol with router alert")

import os
import sys
from addr import *
from scapy.all import *
from scapy.contrib.igmp import *

if len(sys.argv) != 2:
	print("usage: igmp_ra.py Nn")
	exit(2)

N=sys.argv[1]
IF=eval("IF_"+N);
ADDR=eval("ADDR_"+N);

pid=os.getpid()
eid=pid & 0xffff
packet=IP(src=ADDR, dst=ADDR, options=b"\224\004\000\000")/ \
    IGMP(type=0x11)

send(packet, iface=IF)
