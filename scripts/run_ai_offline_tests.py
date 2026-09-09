"""Run the AI unittest suite with a loopback-only Python network guard.

Provider transports are mocked by the tests. This guard catches missing mocks;
it is not an OS sandbox. The packaging subprocess supplies its own network deny
hook; the diagnostic subprocess removes provider configuration and uses closed
loopback ports. No credentials are needed for this suite.
"""
from __future__ import annotations

import ipaddress
import os
from pathlib import Path
import sys
import unittest


def is_loopback(host: object) -> bool:
    if host in ("localhost", b"localhost"):
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def network_guard(event: str, args: tuple) -> None:
    host = None
    if event in ("socket.connect", "socket.sendto"):
        address = args[-1]
        if isinstance(address, tuple):
            host = address[0]
    elif event in ("socket.getaddrinfo", "socket.gethostbyname"):
        host = args[0]
    if host is not None and not is_loopback(host):
        raise RuntimeError("Offline AI tests attempted external networking; mock the provider transport")


def main() -> int:
    for key in list(os.environ):
        if key.upper().startswith(("OPENAI_", "TRIPO_", "TRIPO3D_", "ORCASLICER_AI_")):
            del os.environ[key]
    sys.addaudithook(network_guard)
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root))
    suite = unittest.defaultTestLoader.discover(str(root / "tools" / "ai"), pattern="test_*.py")
    return 0 if unittest.TextTestRunner(verbosity=1).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
