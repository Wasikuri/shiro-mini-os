#!/usr/bin/env python3
"""Tiny TLS test server for the QEMU harness (tests/qemu_test.py).

Usage: tls_server.py <port> <cert.pem> <key.pem> <marker>

Serves one fixed HTML page containing <marker> over TLS, one
connection at a time, forever. Used twice by the harness: on :8443
with a cert from the bundled test CA (guest must accept) and on
:8444 with a cert from an unrelated CA (guest must reject).
Handshake failures (the negative test) are swallowed so the server
keeps accepting.
"""
import socket
import ssl
import sys


def main():
    port = int(sys.argv[1])
    cert, key, marker = sys.argv[2], sys.argv[3], sys.argv[4]
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", port))
    ls.listen(5)
    body = ("<html><head><title>TLS test</title></head><body>"
            f"<h1>{marker}</h1>"
            "<p>served over TLS by the Shiro test harness</p>"
            "</body></html>").encode()
    resp = (b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
            b"Content-Length: " + str(len(body)).encode() +
            b"\r\nConnection: close\r\n\r\n" + body)
    while True:
        try:
            conn, _ = ls.accept()
        except OSError:
            return
        try:
            s = ctx.wrap_socket(conn, server_side=True)
            s.settimeout(15)
            data = b""
            while b"\r\n\r\n" not in data and len(data) < 65536:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            if data:
                s.sendall(resp)
            try:
                s.unwrap()
            except Exception:
                pass
            s.close()
        except Exception:
            try:
                conn.close()
            except Exception:
                pass


if __name__ == "__main__":
    main()
