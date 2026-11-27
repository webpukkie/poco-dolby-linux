#!/usr/bin/env python3
"""Drive qemu -g via RSP: run to SIGTRAP (probe prints base), then breakpoint tracing."""
import socket, sys, time, re

class RSP:
    def __init__(self, port):
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=1); break
            except OSError: time.sleep(0.1)
        else: raise RuntimeError("no connect")
        self.s.settimeout(5.0); self.buf = b""
        self.s.sendall(b"+")
    def _recv(self):
        d = self.s.recv(8192)
        if not d: raise RuntimeError("eof")
        self.buf += d
    def send(self, data):
        b = data.encode()
        self.s.sendall(b"$" + b + b"#" + ("%02x" % (sum(b) & 0xff)).encode())
    def read_pkt(self, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            self.buf = self.buf.lstrip(b"+-")
            while b"$" in self.buf:
                i = self.buf.index(b"$"); j = self.buf.find(b"#", i)
                if j >= 0 and len(self.buf) >= j + 3:
                    p = self.buf[i+1:j]; self.buf = self.buf[j+3:]
                    self.s.sendall(b"+")
                    return p.decode(errors="replace")
            self.s.settimeout(max(0.1, end - time.time()))
            try: self._recv()
            except socket.timeout: break
        return None
    def cmd(self, data, timeout=5):
        self.send(data)
        while True:
            p = self.read_pkt(timeout)
            if p is None: return None
            if p.startswith("$"): continue
            return p

def parse_regs(g):
    vals = []
    for i in range(0, len(g) - 15, 16):
        w = g[i:i+16]
        vals.append(int.from_bytes(bytes.fromhex(w), "little"))
    return vals

def rd32(r, addr):
    p = r.cmd("m%x,4" % addr)
    if p is None or p.startswith("E"): return None
    try: return int.from_bytes(bytes.fromhex(p[:8]), "little", signed=True)
    except Exception: return None

def main():
    port = 1234
    r = RSP(port)
    r.cmd("qSupported")
    r.send("c")
    p = None
    for _ in range(20):
        p = r.read_pkt(60)
        if p and (p.startswith("T") or p.startswith("W") or p.startswith("X")):
            break
        p = None
    print("first stop:", p)
    time.sleep(0.3)
    out = open("/tmp/qemu.out").read()
    m = re.search(r"base=([0-9a-f]+)", out)
    if not m: print(out[-500:]); return
    BASE = int(m.group(1), 16)
    print("BASE=%x" % BASE)

    bps = {
        "EffectDap.process":  0x12a4a0,
        "ldar.lastGain":      0x12a548,
        "setBypass.store":    0x12aac4,
        "setBypass.entry":    0x12aa60,
        "setEnabled.entry":   0x12ab80,
        "updPE.entry":        0x12a8f0,
        "crossfade.call":     0x12a850,
        "xfade.ret":          0x12a868,
        "check38":            0x12a82c,
        "DapXfade38a":        0x139750,
        "DapXfade38b":        0x139948,
        "Dap2Proc.entry":     0x1411a0,
        "prepare":            0x14143c,
        "cpdp_process":       0x14145c,
        "flag50.set":         0x12a6c8,
        "mute.clear":         0x12a64c,
        "unmute.set":         0x12a5b8,
    }
    names = {}
    for n, off in bps.items():
        a = BASE + off
        resp = r.cmd("Z0,%x,1" % a)
        names[a] = n
        if resp != "OK": print("bp fail", n, hex(a), resp)

    at_bp = False
    for it in range(120):
        if at_bp:
            r.send("s")
            q = None
            for _ in range(10):
                q = r.read_pkt(30)
                if q and (q.startswith("T") or q.startswith("W") or q.startswith("X")):
                    break
            if q is None or q.startswith("W") or q.startswith("X"):
                print("exited during step-over:", q); break
            at_bp = False
        r.send("c")
        p = None
        for _ in range(10):
            q = r.read_pkt(120)
            if q and (q.startswith("T") or q.startswith("W") or q.startswith("X")):
                p = q; break
        if p is None:
            print("no stop (timeout)"); break
        if p.startswith("W") or p.startswith("X"):
            print("exited:", p); break
        regs = parse_regs(r.cmd("g") or "")
        pc = regs[32] if len(regs) > 32 else 0
        at_bp = pc in names
        n = names.get(pc, "?%x" % pc)
        extra = ""
        if pc == BASE + 0x12a4a0:
            this = regs[0]
            extra = "this=%x f49=%s f50=%s f51=%s lastGain=%s" % (
                this, rd32(r, this+49), rd32(r, this+50), rd32(r, this+51), rd32(r, this+0x208))
        elif pc in (BASE + 0x12a548, BASE + 0x12a64c, BASE + 0x12a5b8, BASE + 0x12a6c8):
            extra = "x19=%x" % regs[19]
        elif pc == BASE + 0x12aac4:
            extra = "store %d -> f49 (this=%x)" % (regs[21] & 1, regs[19])
        elif pc == BASE + 0x12aa60:
            extra = "this=%x bypassarg=%d" % (regs[0], regs[1] & 1)
        elif pc == BASE + 0x12ab80:
            extra = "this=%x en=%d" % (regs[0], regs[1] & 1)
        elif pc == BASE + 0x12a8f0:
            this = regs[0]
            extra = "this=%x f49=%d" % (this, rd32(r, this + 49) or -1)
        elif pc == BASE + 0x12a868:
            extra = "xfade rc=%d" % (regs[0] & 0xffffffff)
        elif pc == BASE + 0x14143c:
            extra = "prepare entered"
        elif pc == BASE + 0x14145c:
            extra = "cpdp_process entered"
        elif pc in (BASE + 0x139750, BASE + 0x139948):
            extra = "DapXfade -38 site"
        print("stop %s off=%x %s %s" % (p.split(";")[0][:8], pc - BASE, n, extra))
    r.s.close()

if __name__ == "__main__":
    main()
