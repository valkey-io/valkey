#!/usr/bin/env python3
import socket
import threading
import time
import os
import sys

# Directory where control files will be placed
CONTROL_DIR = os.path.dirname(os.path.abspath(__file__))

def should_block(port):
    return os.path.exists(os.path.join(CONTROL_DIR, f"block_{port}"))

def forward(src, dst, port, direction):
    src.setblocking(False)
    dst.setblocking(False)
    blocked_logged = False
    while True:
        if should_block(port):
            if not blocked_logged:
                print(f"[{port}] {direction} traffic BLOCKED")
                blocked_logged = True
            time.sleep(0.05)
            continue
        if blocked_logged:
            print(f"[{port}] {direction} traffic RESUMED")
            blocked_logged = False
        try:
            data = src.recv(4096)
            if not data:
                break
            # Send data to dst
            # Since we set non-blocking, we should handle partial sends if any,
            # but for test simplicity we just do blocking send or wait until writable.
            dst.setblocking(True)
            dst.sendall(data)
            dst.setblocking(False)
        except BlockingIOError:
            time.sleep(0.01)
            continue
        except Exception as e:
            # print(f"[{port}] {direction} connection closed due to: {e}")
            break
    try:
        src.close()
    except:
        pass
    try:
        dst.close()
    except:
        pass

def handle_client(client_socket, target_host, target_port, proxy_port):
    try:
        target_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        target_socket.connect((target_host, target_port))
        print(f"[{proxy_port}] Connected to target {target_host}:{target_port}")
    except Exception as e:
        print(f"[{proxy_port}] Failed to connect to target {target_host}:{target_port}: {e}")
        client_socket.close()
        return

    # Start threads to forward in both directions
    t1 = threading.Thread(target=forward, args=(client_socket, target_socket, proxy_port, "C->T"), daemon=True)
    t2 = threading.Thread(target=forward, args=(target_socket, client_socket, proxy_port, "T->C"), daemon=True)
    t1.start()
    t2.start()

def start_proxy(proxy_port, target_host, target_port):
    proxy_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    proxy_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    proxy_socket.bind(("127.0.0.1", proxy_port))
    proxy_socket.listen(128)
    print(f"Proxy listening on 127.0.0.1:{proxy_port} -> forwarding to {target_host}:{target_port}")

    while True:
        try:
            client_socket, addr = proxy_socket.accept()
            print(f"[{proxy_port}] Accepted connection from {addr}")
            threading.Thread(target=handle_client, args=(client_socket, target_host, target_port, proxy_port), daemon=True).start()
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"Error accepting connection on {proxy_port}: {e}")
            break

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: proxy.py <proxy_port> <target_host> <target_port>")
        sys.exit(1)
    
    proxy_port = int(sys.argv[1])
    target_host = sys.argv[2]
    target_port = int(sys.argv[3])
    
    start_proxy(proxy_port, target_host, target_port)
