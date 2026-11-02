#!/usr/bin/env python3
"""NEXT-STEPS P0 fixture: a host that stalls mid-image.

Accepts a TCP connection, sends a 200 response with a large
Content-Length, then drips one body byte every 3 seconds -- just
inside the download core's 10 s idle timeout, so without a
per-image total deadline the transfer would run to the 120 s
body cap instead of finishing. The guest is expected to give
up after ~5 s (IMG_FETCH_TIMEOUT_S), show the alt text, and
render the rest of the page; when it closes the connection
this server stops dripping and accepts the next one.

Listens on 0.0.0.0:8089 (guest-visible as 10.0.2.2:8089).
Stdlib only.
"""
import socket
import threading
import time

HOST = "0.0.0.0"
PORT = 8089
DRIP_INTERVAL = 3.0
MAX_DRIPS = 60


def handle(conn):
    try:
        conn.settimeout(10)
        try:
            conn.recv(4096)  # the HTTP request; ignore its content
        except socket.timeout:
            pass
        conn.settimeout(None)
        conn.sendall(
            b"HTTP/1.1 200 OK\r\n"
            b"Content-Type: image/png\r\n"
            b"Content-Length: 100000\r\n"
            b"Connection: close\r\n\r\n"
        )
        for _ in range(MAX_DRIPS):
            time.sleep(DRIP_INTERVAL)
            try:
                conn.sendall(b"\x89")
            except OSError:
                break  # guest gave up and closed: stop dripping
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(5)
    print("drip_server listening on %s:%d" % (HOST, PORT), flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
