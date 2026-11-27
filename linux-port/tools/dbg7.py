from rsp import RSP, parse_regs, rd32
import re, time

r = RSP(1234)
r.cmd("qSupported")
r.send("c")
p = None
for _ in range(20):
    p = r.read_pkt(60)
    if p and p[0] in "TWX": break
    p = None
print("first stop:", p)
time.sleep(0.3)
out = open("/tmp/qemu.out").read()
BASE = int(re.search(r"base=([0-9a-f]+)", out).group(1), 16)
print("BASE=%x" % BASE)

sites = {
    0x12a54c: "ldar.after (poke w24 here)",
    0x12a6f4: "noprime.skip-target",
    0x12a82c: "check38",
    0x12a850: "xfade.call",
    0x12a868: "xfade.ret",
    0x1396b4: "state0.main-DSP",
    0x139724: "state4.-38-site",
    0x139948: "state4.-38b",
    0x1411a0: "Dap2Proc.entry",
    0x14143c: "prepare.entry",
    0x14145c: "cpdp_process.entry",
}
for off, n in sites.items():
    resp = r.cmd("Z0,%x,1" % (BASE + off))
    if resp != "OK": print("bp fail", n, resp)

pokes = 0
for it in range(120):
    r.send("c")
    p = None
    for _ in range(10):
        q = r.read_pkt(120)
        if q and q[0] in "TWX": p = q; break
    if p is None:
        print("no stop (timeout)"); break
    if p[0] in "WX":
        print("exited:", p); break
    regs = parse_regs(r.cmd("g") or "")
    pc = regs[32]
    off = pc - BASE
    n = sites.get(off, "?%x" % off)
    extra = ""
    if off == 0x12a54c:
        this = regs[19]
        newgain = rd32(r, regs[31] + 12)
        lastgain = regs[24]
        if lastgain >= 1 << 63: lastgain -= 1 << 64
        f50 = rd32(r, this + 50)
        f49 = rd32(r, this + 49)
        print("poke-site blk%d: lastGain=%d cacheNewGain=%d f49=%d f50=%d -> forcing w24=cache" % (pokes, lastgain, newgain, f49, f50))
        val = (newgain & 0xffffffff).to_bytes(8, "little").hex()
        resp = r.cmd("P%x=%s" % (24, val))
        if resp != "OK": print("P24 fail:", resp)
        pokes += 1
        r.send("s"); q = r.read_pkt(30)
        if q and q[0] in "WX": print("exit-step", q); break
        continue
    elif off == 0x12a868:
        rc = regs[0]
        if rc >= 1 << 63: rc -= 1 << 64
        extra = "rc=%d" % rc
    elif off == 0x12a850:
        extra = "in_prov=%x out_prov=%x" % (regs[1], regs[2])
    elif off == 0x12a82c:
        this = regs[19]
        extra = "f49=%s f50=%s f51=%s" % (rd32(r, this+49), rd32(r, this+50), rd32(r, this+51))
    elif off == 0x1396b4:
        extra = "main DSP path entered"
    elif off == 0x12a6f4:
        extra = "priming skipped (w24==cache)"
    print("stop %s off=%x %s %s" % (p.split(";")[0][:8], off, n, extra))
    r.send("s"); q = r.read_pkt(30)
    if q and q[0] in "WX": print("exit-step", q); break
print("DONE pokes=%d" % pokes)
