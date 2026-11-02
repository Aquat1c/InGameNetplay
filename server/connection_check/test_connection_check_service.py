"""Run with: python test_connection_check_service.py"""
import socket
import threading

import connection_check_service as svc


def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(1)
    return s


def scenario(answer_ping, answer_punch, stranger=False):
    """Fake host and relay on loopback; run_check must only trust the right packets."""
    service, host, relay, other = udp(), udp(), udp(), udp()
    service_addr = service.getsockname()
    seen = {}

    def fake_host():
        try:
            data, _ = host.recvfrom(64)
            seen["ping"] = data
            if stranger:
                other.sendto(b"\x00\x03\x18\x01", service_addr)
            if answer_ping:
                host.sendto(b"\x00\x03\x18\x01", service_addr)
        except socket.timeout:
            pass

    def fake_relay():
        try:
            data, _ = relay.recvfrom(64)
            seen["punch"] = data
            relay.sendto(b"\x00\x0a\x0a\x1c" + b"\0" * 28, service_addr)
            if answer_punch:
                host.sendto(b"\x00\x02\x18\x01", service_addr)
        except socket.timeout:
            pass

    threads = [threading.Thread(target=fake_host), threading.Thread(target=fake_relay)]
    for t in threads:
        t.start()
    result = svc.run_check(service, relay.getsockname(), "127.0.0.1", host.getsockname()[1],
                           ping_wait=0.3, relay_wait=0.3)
    for t in threads:
        t.join()
    assert seen["ping"][:3] == b"\x00\x02\x18"
    assert seen["punch"][:4] == b"\x00\x0a\x0a\x1c" and len(seen["punch"]) == 32
    assert seen["punch"][4:6] == b"\x02\x00"
    assert seen["punch"][6:8] == host.getsockname()[1].to_bytes(2, "big")
    assert seen["punch"][8:12] == socket.inet_aton("127.0.0.1")
    return result


assert scenario(True, True) == (True, True)
assert scenario(False, True) == (False, True)
assert scenario(True, False) == (True, False)
assert scenario(False, False) == (False, False), "relay reply alone must not pass"
assert scenario(False, False, stranger=True) == (False, False), "a stranger's Pong must not pass"

assert svc.parse_port("10800") == 10800
for bad in ("", "abc", "80", "65536", "1080000", "-1"):
    assert svc.parse_port(bad) is None, bad

assert svc.is_testable_ip("8.8.8.8")
for bad in ("", "nonsense", "127.0.0.1", "192.168.0.6", "10.0.0.1", "100.64.0.1", "2001:4860:4860::8888"):
    assert not svc.is_testable_ip(bad), bad

now = [100.0]
limiter = svc.RateLimiter(15, clock=lambda: now[0])
assert limiter.allow("a") and not limiter.allow("a") and limiter.allow("b")
now[0] += 15
assert limiter.allow("a")

sock = udp()
service = svc.Service(sock, ("127.0.0.1", 9))
assert service.handle(svc.BASE_PATH + "/status", {}, "")[1]["state"] == "ready"
assert service.handle("/other", {}, "8.8.8.8")[0] == 404
assert service.handle(svc.BASE_PATH + "/run", {"port": ["bad"]}, "8.8.8.8")[0] == 400
assert service.handle(svc.BASE_PATH + "/run", {"port": ["10800"]}, "192.168.0.6")[1]["reason"] == "address"
print("connection check service: PASS")
