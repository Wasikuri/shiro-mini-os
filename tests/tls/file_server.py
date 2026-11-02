#!/usr/bin/env python3
"""Tiny TLS file server for the QEMU harness (tests/qemu_test.py).

Usage: file_server.py <port> <cert.pem> <key.pem> <file>

Serves <file> (raw bytes, Content-Length set) over TLS at any
path, one connection at a time, forever. Used by the "apps" check
to exercise `install https://...` against the bundled test CA.
"""
import socket
import ssl
import sys


def main():
    port = int(sys.argv[1])
    cert, key, path = sys.argv[2], sys.argv[3], sys.argv[4]
    with open(path, "rb") as f:
        body = f.read()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", port))
    ls.listen(5)
    resp = (b"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
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
