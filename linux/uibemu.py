#!/usr/bin/env python3
"""
UIB Box emulator for the Innovative Foto "Portrait Studio" vend app (v8mnu22b).

Wire protocol (reversed from /vend/bin/uibtest, "UIBBOX v1.04 pv23/115200"):
  18-byte frames:
    [0]    '<' (0x3C) magic
    [1]    protocol version
    [2:4]  nonce (big-endian), used as the cipher IV
    [4:18] 14 bytes scrambled with a Wichmann-Hill keystream (seed 0x3021),
           chained per big-endian 16-bit word.  Plaintext layout:
             [4:6]  0
             [6:8]  checksum of [8:18]: v = ror16(v,1) + byte
             [8]    msgid
             [9]    command / reply type
             [10:18] 8 bytes of payload
  Host sends a command -> box replies ACK (same msgid).
  Host sends STROBE (4) -> box replies with last command's msgid, cmd code and result payload.
  Bad frame -> box replies ERR (3); host resyncs by stuffing single 0 bytes.

Usage (as root in WSL):  python3 -I uibemu.py /path/to/rootfs/dev/uib
Keyboard: digits 1-8 press button N, 'c' inserts a coin/token, 'q' quits.
"""
import os, pty, sys, tty, select, termios, time, struct, random

CMDS = {1: "ACK", 2: "NAK", 3: "ERR", 4: "STROBE", 5: "PING", 8: "NOOP", 9: "ECHO",
        11: "GET_VERSION", 12: "GET_FEATURES", 13: "GET_STATUS", 14: "ENABLE_BILL_LINES",
        15: "ENABLE_COIN_LINES", 16: "GET_BILL_COUNTS", 17: "GET_COIN_COUNTS",
        18: "GET_BUTTONS", 19: "SET_LIGHTS", 20: "SET_EJECTOR", 21: "SET_PORT",
        22: "SET_LOCATION", 23: "ARM_WATCHDOG", 24: "SET_TILT", 25: "SET_SPARE1",
        26: "SET_SPARE2", 27: "MATCH_PROTOCOL", 28: "ENABLE_CHUCKETOKEN_LINES"}

SEED = 0x3021
PROTO = 23


class WH:
    def __init__(self, seed):
        self.a = self.b = self.c = seed & 0xFFFF

    def next(self):
        self.c = (self.c * 0xAB) % 0x763D
        self.b = (self.b * 0xAC) % 0x7663
        self.a = (self.a * 0xAA) % 0x7673
        return (self.a ^ self.b ^ self.c) & 0xFFFF


def crypt(buf, iv, encrypt):
    """In-place scramble of 14 bytes (7 BE words) - mirrors sub_8049286."""
    g = WH(SEED)
    g.next()  # seeding call also advances
    chain = iv & 0xFFFF
    out = bytearray(buf)
    for i in range(0, len(out) - 1, 2):
        k = g.next()
        w = (out[i] << 8) | out[i + 1]
        if encrypt:
            o = (k ^ chain ^ (~w & 0xFFFF)) & 0xFFFF
            chain = o
        else:
            o = (~(k ^ chain ^ w)) & 0xFFFF
            chain = w
        out[i], out[i + 1] = o >> 8, o & 0xFF
    return out


def checksum(b10):
    v = 0
    for x in b10:
        v = (((v >> 1) | (v << 15)) & 0xFFFF) + x
        v &= 0xFFFF
    return v


def decode(frame):
    if frame[0] != 0x3C:
        return None, "bad magic %02x" % frame[0]
    nonce = (frame[2] << 8) | frame[3]
    plain = frame[:4] + crypt(frame[4:18], nonce, False)
    cs = checksum(plain[8:18])
    if cs != ((plain[6] << 8) | plain[7]):
        return None, "checksum %04x != %02x%02x" % (cs, plain[6], plain[7])
    return plain, None


_nonce = random.randint(1, 0xFFFF)


def encode(msgid, cmd, payload=b""):
    global _nonce
    _nonce = (_nonce * 1009) & 0xFFFF or 1
    p = bytearray(18)
    p[8] = msgid & 0xFF
    p[9] = cmd & 0xFF
    pl = bytes(payload)[:8]
    p[10:10 + len(pl)] = pl
    cs = checksum(p[8:18])
    p[6], p[7] = cs >> 8, cs & 0xFF
    out = bytearray(4)
    out[0], out[1], out[2], out[3] = 0x3C, PROTO, _nonce >> 8, _nonce & 0xFF
    return bytes(out + crypt(p[4:18], _nonce, True))


class Box:
    def __init__(self, log):
        self.log = log
        self.last = (0, 4, bytes(8))  # msgid, cmd, result
        self.buttons = 0              # latched bitmask, buttons 1-16, cleared by GET_BUTTONS
        self.coins = [0] * 8          # pending counts per coin line, cleared by GET_COIN_COUNTS
        self.lights = 0
        self.quiet = {13, 4}          # don't log the constant status polling

    def press(self, n):
        self.buttons |= 1 << (n - 1)

    def coin(self, line=1):
        self.coins[line - 1] = min(255, self.coins[line - 1] + 1)

    def result_for(self, cmd, payload):
        if cmd == 11:   # GET_VERSION -> 2 BE words read by host
            return bytes([0x01, 0x04, 0x00, PROTO, 0, 0, 0, 0])
        if cmd == 12:   # GET_FEATURES -> two dwords
            return bytes([0xFF] * 8)
        if cmd == 13:   # GET_STATUS: [0] 0x20 coin pending / 0x04 bill pending, [1] bit0 buttons pending
            s0 = 0x20 if any(self.coins) else 0
            s1 = 0x01 if self.buttons else 0
            return bytes([s0, s1, 0, 0, 0, 0, 0, 0])
        if cmd == 16:   # GET_BILL_COUNTS
            return bytes(8)
        if cmd in (17,):  # GET_COIN_COUNTS: one byte per line
            r = bytes(self.coins)
            self.coins = [0] * 8
            return r
        if cmd == 18:   # GET_BUTTONS: bytes = bitmask buttons 1-8, 9-16
            b = self.buttons
            self.buttons = 0
            return bytes([b & 0xFF, b >> 8, 0, 0, 0, 0, 0, 0])
        if cmd == 19:
            self.lights = payload[0]
        if cmd in (9, 27):  # ECHO / MATCH_PROTOCOL
            return bytes(payload)
        return bytes(8)

    def handle(self, plain):
        msgid, cmd, payload = plain[8], plain[9], bytes(plain[10:18])
        if cmd == 4:  # STROBE: return result of previous command, type ACK
            m, c, r = self.last
            return encode(m, 1, r)
        res = self.result_for(cmd, payload)
        self.last = (msgid, cmd, res)
        if cmd not in self.quiet or any(res):
            self.log("%-26s msgid=%3d payload=%s -> result=%s" %
                     (CMDS.get(cmd, "cmd%d" % cmd), msgid, payload.hex(), res.hex()))
        return encode(msgid, 1)


def main():
    link = sys.argv[1] if len(sys.argv) > 1 else None
    logf = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "uibemu.log"), "a", buffering=1)

    def log(s):
        line = time.strftime("%H:%M:%S ") + s
        print(line, flush=True)
        logf.write(line + "\n")

    master, slave = pty.openpty()
    tty.setraw(slave)
    tty.setraw(master)
    spath = os.ttyname(slave)
    log("UIB emulator on %s" % spath)
    if link:
        if not os.path.exists(link):
            open(link, "w").close()
        os.system("umount %s 2>/dev/null; mount --bind %s %s" % (link, spath, link))
        log("bound %s -> %s" % (spath, link))

    box = Box(log)
    buf = bytearray()
    # control FIFO: echo "b 3" (press button 3) or "c" / "c 2" (coin on line 2) > /tmp/uibemu.ctl
    ctl_path = "/tmp/uibemu.ctl"
    if not os.path.exists(ctl_path):
        os.mkfifo(ctl_path)
    ctl = os.open(ctl_path, os.O_RDWR | os.O_NONBLOCK)
    stdin_tty = sys.stdin.isatty()
    old = termios.tcgetattr(0) if stdin_tty else None
    if stdin_tty:
        tty.setcbreak(0)
    try:
        while True:
            fds = [master, ctl] + ([0] if stdin_tty else [])
            r, _, _ = select.select(fds, [], [], 0.5)
            if ctl in r:
                for line in os.read(ctl, 1024).decode(errors="ignore").splitlines():
                    p = line.split()
                    if p and p[0] == "b" and len(p) > 1:
                        box.press(int(p[1]))
                        log("** button %s pressed (ctl)" % p[1])
                    elif p and p[0] == "c":
                        box.coin(int(p[1]) if len(p) > 1 else 1)
                        log("** coin inserted (ctl)")
            if 0 in r:
                ch = os.read(0, 1).decode(errors="ignore")
                if ch == "q":
                    break
                if ch.isdigit() and ch != "0":
                    box.press(int(ch))
                    log("** button %s pressed" % ch)
                if ch == "c":
                    box.coin(1)
                    log("** coin inserted on line 1")
            if master in r:
                try:
                    data = os.read(master, 256)
                except OSError:
                    time.sleep(0.1)
                    continue
                buf += data
                while len(buf) >= 18:
                    frame, buf = bytes(buf[:18]), buf[18:]
                    plain, err = decode(frame)
                    if plain is None:
                        log("bad frame (%s): %s -> ERR" % (err, frame.hex()))
                        os.write(master, encode(0, 3))
                        continue
                    os.write(master, box.handle(plain))
    finally:
        if stdin_tty:
            termios.tcsetattr(0, termios.TCSADRAIN, old)
        if link:
            os.system("umount %s 2>/dev/null" % link)


if __name__ == "__main__":
    main()
