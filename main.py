import socket
import threading
import select
import json

# Load configuration from conf.json
with open('conf.json', 'r') as config_file:
    config = json.load(config_file)

# Proxy server configuration from conf.json
HOST = config.get('host', '127.0.0.1')
PORT = config.get('port', 80)
TARGET_HOST = config.get('target_host', '127.0.0.1')
TARGET_PORTS = config.get('target_port', [3000])  # List of target ports
BUFFER_SIZE = config.get('buffer_size', 4096)

# Global index to track the current target port for round-robin
current_target_index = 0
lock = threading.Lock()  # Lock to prevent race condition on index

def get_next_target_port():
    global current_target_index
    with lock:  # Ensure only one thread accesses the index at a time
        port = TARGET_PORTS[current_target_index]
        current_target_index = (current_target_index + 1) % len(TARGET_PORTS)
    return port

def handle_client(client_socket):
    try:
        request = client_socket.recv(BUFFER_SIZE)
        request_str = request.decode('utf-8')

        # Check if it's a WebSocket upgrade request
        if "Upgrade: websocket" in request_str:
            handle_websocket(client_socket, request)
        else:
            print(f"Received request:\n{request_str}")

            # Get the next target port in round-robin fashion
            target_port = get_next_target_port()
            print(f"Forwarding request to {TARGET_HOST}:{target_port}")

            # Connect to the target server
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as proxy_socket:
                proxy_socket.connect((TARGET_HOST, target_port))
                proxy_socket.send(request)

                while True:
                    # Use select to handle multiple connections
                    rlist, _, _ = select.select([proxy_socket, client_socket], [], [])
                    for r in rlist:
                        data = r.recv(BUFFER_SIZE)
                        if len(data) > 0:
                            if r is proxy_socket:
                                client_socket.send(data)
                            else:
                                proxy_socket.send(data)
                        else:
                            return
    except ConnectionRefusedError as e:
        print(f"Connection to target server failed: {e}")
    finally:
        client_socket.close()

def handle_websocket(client_socket, initial_request):
    try:
        # Get the next target port in round-robin fashion
        target_port = get_next_target_port()
        print(f"Forwarding WebSocket request to {TARGET_HOST}:{target_port}")

        # Connect to the target server
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as proxy_socket:
            proxy_socket.connect((TARGET_HOST, target_port))
            proxy_socket.send(initial_request)

            # WebSocket traffic is bidirectional, so we need to forward in both directions
            def forward(source, destination):
                while True:
                    data = source.recv(BUFFER_SIZE)
                    if len(data) == 0:
                        break
                    destination.send(data)

            client_to_server = threading.Thread(target=forward, args=(client_socket, proxy_socket))
            server_to_client = threading.Thread(target=forward, args=(proxy_socket, client_socket))

            client_to_server.start()
            server_to_client.start()

            client_to_server.join()
            server_to_client.join()
    except Exception as e:
        print(f"WebSocket handling error: {e}")
    finally:
        client_socket.close()

def start_server():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server_socket:
        server_socket.bind((HOST, PORT))
        server_socket.listen(5)
        print(f"Proxy server is listening on {HOST}:{PORT}")

        while True:
            client_socket, addr = server_socket.accept()
            print(f"Accepted connection from {addr}")
            client_handler = threading.Thread(target=handle_client, args=(client_socket,))
            client_handler.start()

if __name__ == "__main__":
    start_server()
