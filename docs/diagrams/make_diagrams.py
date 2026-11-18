#!/usr/bin/env python3
"""Generate the state-machine diagrams of the protocol specification.
Edges carry short numbered labels; the numbers refer to the transition
tables in docs/protocol_spec.md. Requires Graphviz (dot)."""
import os
import subprocess

os.chdir(os.path.dirname(os.path.abspath(__file__)))   # write next to this script

HEAD = '''digraph G {
  graph [fontname="DejaVu Sans", fontsize=11, rankdir=TB, nodesep=0.9, ranksep=0.65, pad=0.25];
  node  [fontname="DejaVu Sans", fontsize=11, shape=box, style="rounded,filled",
         fillcolor="#EEF3FA", color="#4C72B0", penwidth=1.3, margin="0.18,0.08"];
  edge  [fontname="DejaVu Sans", fontsize=10, color="#555555", arrowsize=0.7];
  start [shape=circle, label="", width=0.16, style=filled, fillcolor=black];
'''
OK = 'shape=doublecircle, fillcolor="#E3F1E6", color="#55A868"'
BAD = 'shape=doublecircle, fillcolor="#F8E1E1", color="#C44E52"'
RED = 'color="#C44E52", fontcolor="#C44E52"'

DIAGRAMS = {
"sender_gbn": '''
  SEND [label="SENDING\\nwindow has room and data left"];
  WAIT [label="WAITING\\nwindow full or all data sent"];
  FIN  [label="CLOSING\\nFIN sent, waiting for FINACK"];
  OK   [label="DONE\\nexit 0", %(OK)s];
  FAIL [label="GAVE UP\\nexit 2", %(BAD)s];
  start -> SEND [label=" 0  init"];
  SEND:e -> SEND:e [label=" 1  send new packet"];
  SEND -> WAIT [label=" 2  window full / no data left"];
  WAIT -> SEND [label=" 3  ACK of new data"];
  WAIT:e -> WAIT:e [label=" 4  timeout:\\n resend whole window", %(RED)s];
  WAIT:w -> WAIT:w [label="5  old ACK /\\nbad CRC "];
  WAIT -> FIN  [label=" 6  all N packets ACKed"];
  FIN:e -> FIN:e [label=" 7  timeout: resend FIN", %(RED)s];
  FIN  -> OK   [label=" 8  FINACK"];
  FIN  -> FAIL [label=" 9  too many FIN attempts"];
''',
"sender_sr": '''
  SEND [label="SENDING\\nwindow has room and data left"];
  WAIT [label="WAITING\\nwindow full or all data sent"];
  FIN  [label="CLOSING\\n(as Go-Back-N, transitions 7-9)"];
  OK   [label="DONE\\nexit 0", %(OK)s];
  start -> SEND [label=" 0  init"];
  SEND:e -> SEND:e [label=" 1  send new packet,\\n start its own timer"];
  SEND -> WAIT [label=" 2  window full / no data left"];
  WAIT -> SEND [label=" 3  ACK of an unACKed packet"];
  WAIT:e -> WAIT:e [label=" 4  timer of packet i:\\n resend packet i only", %(RED)s];
  WAIT:w -> WAIT:w [label="5  old / repeated ACK,\\nbad CRC "];
  WAIT -> FIN  [label=" 6  all N packets ACKed"];
  FIN  -> OK   [label=" 8  FINACK"];
''',
"receiver_gbn": '''
  RECV [label="RECEIVING\\nE = next expected packet"];
  LING [label="LINGER\\nfile complete"];
  OK   [label="DONE\\nexit 0", %(OK)s];
  FAIL [label="INCOMPLETE\\nexit 3", %(BAD)s];
  start -> RECV [label=" 0  E = 0"];
  RECV:e -> RECV:e [label=" 1  expected DATA:\\n deliver, ACK it"];
  RECV:w -> RECV:w [label="2  other DATA: discard,\\nre-ACK E-1\\n3  bad CRC / early FIN "];
  RECV -> LING [label=" 4  FIN with seq E"];
  LING:e -> LING:e [label=" 5  FIN again / DATA:\\n answer again"];
  LING -> OK   [label=" 6  linger time over"];
  RECV -> FAIL [label=" 7  idle timeout"];
''',
"receiver_sr": '''
  RECV [label="RECEIVING\\nwindow [E, E+W)"];
  LING [label="LINGER\\n(as Go-Back-N, transitions 5-6)"];
  OK   [label="DONE\\nexit 0", %(OK)s];
  FAIL [label="INCOMPLETE\\nexit 3", %(BAD)s];
  start -> RECV [label=" 0  E = 0, buffer empty"];
  RECV:e -> RECV:e [label=" 1  DATA in window:\\n buffer, ACK it,\\n deliver in order"];
  RECV:w -> RECV:w [label="2  DATA in [E-W, E): re-ACK it\\n3  other DATA / bad CRC /\\nearly FIN: ignore "];
  RECV -> LING [label=" 4  FIN with seq E"];
  LING -> OK   [label=" 6  linger time over"];
  RECV -> FAIL [label=" 7  idle timeout"];
''',
}

for name, body in DIAGRAMS.items():
    src = HEAD + body % dict(OK=OK, BAD=BAD, RED=RED) + "}\n"
    with open(name + ".dot", "w") as f:
        f.write(src)
    for fmt in ("png", "svg"):
        subprocess.run(["dot", "-T" + fmt, "-Gdpi=180", name + ".dot", "-o", f"{name}.{fmt}"],
                       check=True)
    print("wrote", name)
