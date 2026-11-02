#!/usr/bin/env python3
"""In-game netplay connection check. Runs behind Caddy; stdlib only.

Caddy must set X-EFZ-Client-IP from the real remote address. Probes only ever
go to that address, so the service cannot be aimed at someone else.
"""
import argparse
import ipaddress
import json
import select
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

SCHEMA = "efz-connection-check/1"
BASE_PATH = "/api/connection-check/v1"
TYPE_PING = 2
TYPE_PONG = 3
PING_WAIT_S = 2.0
RELAY_WAIT_S = 3.0
MIN_INTERVAL_S = 15.0
LOCK_WAIT_S = 8.0


def build_ping():
    stamp = int(time.time() * 1000) & 0x7FFFFFFF
    out = bytearray(b"\x00\x02\x18")
    while stamp >= 0x80:
        out.append(stamp & 0x7F | 0x80)
        stamp >>= 7
    out.append(stamp)
    return bytes(out)


def build_punch(ip, port):
    endpoint = struct.pack("<H", 2) + struct.pack(">H", port) + socket.inet_aton(ip) + b"\0" * 20
    return b"\x00\x0a\x0a\x1c" + endpoint


def wait_for(sock, seconds, matches):
    end = time.monotonic() + seconds
    while True:
        left = end - time.monotonic()
        if left <= 0:
            return False
        ready, _, _ = select.select([sock], [], [], left)
        if not ready:
            return False
        try:
            data, src = sock.recvfrom(2048)
        except OSError:
            continue
        if len(data) >= 2 and matches(data[1], src):
            return True


def drain(sock):
    while select.select([sock], [], [], 0)[0]:
        try:
            sock.recvfrom(2048)
        except OSError:
            pass


def run_check(sock, relay, ip, port, ping_wait=PING_WAIT_S, relay_wait=RELAY_WAIT_S):
    """One Ping (Pong = direct works), then one Punch (Ping from the caller = relay works)."""
    drain(sock)
    sock.sendto(build_ping(), (ip, port))
    direct = wait_for(sock, ping_wait, lambda kind, src: kind == TYPE_PONG and src == (ip, port))
    sock.sendto(build_punch(ip, port), relay)
    # The relay's own reply proves nothing: it answers from old entries too.
    relayed = wait_for(sock, relay_wait, lambda kind, src: kind == TYPE_PING and src[0] == ip)
    return direct, relayed


def is_testable_ip(text):
    try:
        address = ipaddress.ip_address(text)
    except ValueError:
        return False
    return address.version == 4 and address.is_global


def parse_port(text):
    if not text.isdigit() or len(text) > 5:
        return None
    port = int(text)
    return port if 1024 <= port <= 65535 else None


class RateLimiter:
    def __init__(self, interval, clock=time.monotonic):
        self.interval = interval
        self.clock = clock
        self.lock = threading.Lock()
        self.last = {}

    def allow(self, key):
        now = self.clock()
        with self.lock:
            self.last = {k: t for k, t in self.last.items() if now - t < self.interval}
            if key in self.last:
                return False
            self.last[key] = now
            return True


class Service:
    def __init__(self, sock, relay):
        self.sock = sock
        self.relay = relay
        # One check at a time, since all replies arrive on one socket.
        self.lock = threading.Lock()
        self.limiter = RateLimiter(MIN_INTERVAL_S)

    def handle(self, path, query, client_ip):
        if path == BASE_PATH + "/status":
            return 200, {"schema": SCHEMA, "state": "ready"}
        if path != BASE_PATH + "/run":
            return 404, {"schema": SCHEMA, "state": "not-found"}
        port = parse_port(query.get("port", [""])[0])
        if port is None:
            return 400, {"schema": SCHEMA, "state": "untested", "reason": "port"}
        if not is_testable_ip(client_ip):
            return 200, {"schema": SCHEMA, "state": "untested", "reason": "address"}
        if not self.limiter.allow(client_ip):
            return 429, {"schema": SCHEMA, "state": "untested", "reason": "rate"}
        if not self.lock.acquire(timeout=LOCK_WAIT_S):
            return 503, {"schema": SCHEMA, "state": "untested", "reason": "busy"}
        try:
            direct, relayed = run_check(self.sock, self.relay, client_ip, port)
        finally:
            self.lock.release()
        return 200, {"schema": SCHEMA, "state": "done", "direct": direct, "relay": relayed}


def make_handler(service):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        timeout = 15

        def do_GET(self):
            parts = urlsplit(self.path)
            status, body = service.handle(
                parts.path, parse_qs(parts.query), self.headers.get("X-EFZ-Client-IP", ""))
            payload = json.dumps(body).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)

        def log_message(self, fmt, *args):
            pass

    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen-port", type=int, default=3211)
    parser.add_argument("--udp-port", type=int, default=10831)
    parser.add_argument("--relay", default="128.199.38.97:10800")
    args = parser.parse_args()
    relay_host, relay_port = args.relay.rsplit(":", 1)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.udp_port))
    service = Service(sock, (relay_host, int(relay_port)))
    server = ThreadingHTTPServer(("127.0.0.1", args.listen_port), make_handler(service))
    server.daemon_threads = True
    server.serve_forever()


if __name__ == "__main__":
    main()
