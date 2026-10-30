#!/usr/bin/env python3
"""Networking (#253).

1. A small HTTP server (user/linux/httpd.c: epoll, accept4) runs in the
   machine on QEMU's e1000 with user-mode networking, its port forwarded to
   the host. The host sends it requests while it records, one at a time and
   four at once, for a fixed page, a page with a counter, a random number and
   the clock, and a 64 KiB page that takes many TCP segments. After the last
   request the card's link is taken down (QEMU monitor: set_link off), and
   the self-test replays every retained epoch from the journal: the received
   frames come from the journal, the replay transmits nothing, and it must be
   byte-exact.
2. A loopback client and server inside the machine (user/linux/netloop.c):
   200000 bytes over TCP on 127.0.0.1 and 20 UDP datagrams, recorded and
   replayed byte-exact.

Usage: tests/qemu/net_gate.py
"""

import http.client
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
KERNEL = os.environ.get("LAPLACE_KERNEL", os.path.join(ROOT, "build", "laplace.elf"))
FAILS = 0


def check(good, what):
    global FAILS
    print(("[ok] " if good else "[FAIL] ") + what, flush=True)
    if not good:
        FAILS += 1
    return good


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def get(port, path):
    """One request without a proxy (the environment's HTTP proxy is not for
    the machine): (status, body)."""
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
    c.request("GET", path)
    r = c.getresponse()
    body = r.read()
    c.close()
    return r.status, body


def http_server():
    port, mon = free_port(), free_port()
    fd, console = tempfile.mkstemp(suffix=".log")
    os.close(fd)
    q = subprocess.Popen(
        [QEMU, "-m", "512M", "-display", "none", "-no-reboot",
         "-kernel", KERNEL,
         "-append", "run=musl_httpd interval=20 keyframes=64 selftest exit",
         "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
         "-netdev", f"user,id=n0,hostfwd=tcp:127.0.0.1:{port}-:8080", "-device", "e1000,netdev=n0",
         "-monitor", f"tcp:127.0.0.1:{mon},server=on,wait=off",
         "-serial", f"file:{console}", "-serial", "null", "-serial", "null"],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, cwd=ROOT)

    def text():
        return open(console, errors="replace").read().replace("\r", "") if os.path.exists(console) else ""

    try:
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline and "httpd: listening" not in text():
            time.sleep(0.2)
        check("httpd: listening on port 8080" in text(), "httpd: the server listens on the machine's e1000")
        st, body = get(port, "/hello")
        check(st == 200 and body == b"hello from Laplace\n", "httpd: GET /hello from the host")
        counts = []
        for _ in range(3):
            st, body = get(port, "/count")
            counts.append(body.decode(errors="replace").strip())
        print("  [host] " + " | ".join(counts), flush=True)
        check(all(re.match(r"served \d+ random \d+ clock \d+$", c) for c in counts),
              "httpd: GET /count three times (a counter, a random number, the clock)")
        st, big = get(port, "/big")
        want = bytes(ord("a") + (i * 7) % 26 for i in range(65536))
        check(st == 200 and big == want, f"httpd: GET /big: {len(big)} bytes intact over many TCP segments")
        results = []

        def one():
            try:
                results.append(get(port, "/count")[0])
            except Exception as e:  # noqa: BLE001
                results.append(str(e))
        threads = [threading.Thread(target=one) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        check(results == [200] * 4, f"httpd: four requests at once ({results})")
        st, _ = get(port, "/quit")
        check(st == 200, "httpd: GET /quit")
        # The network goes away before the replay.
        with socket.create_connection(("127.0.0.1", mon), timeout=10) as m:
            m.sendall(b"set_link n0 off\n")
            time.sleep(0.5)
        q.wait(timeout=1800)
        out = text()
        for line in out.splitlines():
            if re.search(r"^httpd: (request|\d+ requests)|selftest: (inputs|verify rc|first divergence|leak|PASSED|FAILED)", line):
                print("  [httpd] " + line.strip()[:150], flush=True)
        check("httpd: 10 requests, exiting" in out, "httpd: the server answered all 10 and exited")
        m = re.search(r"selftest: inputs: (\d+) records", out)
        check(m is not None and int(m.group(1)) > 50, "httpd: the received frames are journaled inputs"
              + (f" ({m.group(1)} records)" if m else ""))
        m = re.search(r"selftest: verify rc=0: (\d+) epochs replayed, 0 diverged", out)
        check(m is not None, "httpd: the recording replays byte-exact with the link down"
              + (f" ({m.group(1)} epochs)" if m else ""))
        check("selftest: PASSED" in out, "httpd: the self-test passes")
    finally:
        q.kill()
        os.unlink(console)


def loopback():
    r = subprocess.run(
        [QEMU, "-m", "512M", "-display", "none", "-no-reboot",
         "-kernel", KERNEL,
         "-append", "run=musl_netloop interval=5 keyframes=64 selftest exit",
         "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
         "-serial", "stdio", "-serial", "null", "-serial", "null"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=900, cwd=ROOT)
    out = r.stdout.decode(errors="replace").replace("\r", "")
    for line in out.splitlines():
        if re.search(r"^netloop|selftest: (window|verify rc|first divergence|leak|PASSED|FAILED)", line):
            print("  [netloop] " + line.strip()[:150], flush=True)
    check(re.search(r"netloop client: port \d+, sent 200000, got 200000 back, 0 wrong, \d+ polls; "
                    r"20 of 20 datagrams answered", out) is not None,
          "loopback: 200000 bytes over TCP on 127.0.0.1 come back intact; 20 UDP datagrams answered")
    check("netloop: client 0, server 0" in out, "loopback: client and server exit 0")
    check(re.search(r"selftest: verify rc=0: \d+ epochs replayed, 0 diverged", out) is not None,
          "loopback: every epoch replays byte-exact")
    check("selftest: PASSED" in out, "loopback: the self-test passes")


def main():
    if not os.path.exists(KERNEL):
        subprocess.run(["make", "-s"], cwd=ROOT, check=True)
    http_server()
    loopback()
    if FAILS:
        print(f"FAILED: {FAILS} network checks")
        sys.exit(1)
    print("PASSED: an HTTP server answers the host while recording, and it and a loopback pair replay byte-exact")


if __name__ == "__main__":
    main()
