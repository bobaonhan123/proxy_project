#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <cstring>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include "nlohmann/json.hpp" 

using json = nlohmann::json;

const int BUFFER_SIZE = 4096;
std::vector<std::pair<std::string, int>> targets;
int current_target_index = 0;
std::string host="127.0.0.1";
int port = 80;
std::mutex target_lock;
std::atomic<bool> running(true); 

std::pair<std::string, int> get_next_target() {
    std::lock_guard<std::mutex> lock(target_lock);
    std::pair<std::string, int> target = targets[current_target_index];
    current_target_index = (current_target_index + 1) % targets.size();
    return target;
}

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE];

    // Receive request from client
    ssize_t received = recv(client_socket, buffer, BUFFER_SIZE, 0);
    if (received <= 0) {
        close(client_socket);
        return;
    }
    std::string request(buffer, received);
    
    auto [target_host, target_port] = get_next_target();
    std::cout << "Forwarding request to " << target_host << ":" << target_port << std::endl;

    int proxy_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socket == -1) {
        std::cerr << "Failed to create socket to target server.\n";
        close(client_socket);
        return;
    }
    
    sockaddr_in target_addr{};
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(target_port);
    inet_pton(AF_INET, target_host.c_str(), &target_addr.sin_addr);

    if (connect(proxy_socket, (struct sockaddr*)&target_addr, sizeof(target_addr)) < 0) {
        std::cerr << "Connection to target server failed.\n";
        close(client_socket);
        close(proxy_socket);
        return;
    }

    send(proxy_socket, buffer, received, 0);

    fd_set read_fds;
    while (true) {
        FD_ZERO(&read_fds);
        FD_SET(client_socket, &read_fds);
        FD_SET(proxy_socket, &read_fds);
        int max_fd = std::max(client_socket, proxy_socket) + 1;

        select(max_fd, &read_fds, nullptr, nullptr, nullptr);

        if (FD_ISSET(client_socket, &read_fds)) {
            ssize_t len = recv(client_socket, buffer, BUFFER_SIZE, 0);
            if (len <= 0) break;
            send(proxy_socket, buffer, len, 0);
        }
        if (FD_ISSET(proxy_socket, &read_fds)) {
            ssize_t len = recv(proxy_socket, buffer, BUFFER_SIZE, 0);
            if (len <= 0) break;
            send(client_socket, buffer, len, 0);
        }
    }

    close(client_socket);
    close(proxy_socket);
}

void start_server(const std::string& host, int port) {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        std::cerr << "Failed to create server socket.\n";
        return;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Binding server socket failed.\n";
        close(server_socket);
        return;
    }

    if (listen(server_socket, 5) < 0) {
        std::cerr << "Listening on server socket failed.\n";
        close(server_socket);
        return;
    }

    std::cout << "Proxy server listening on " << host << ":" << port << std::endl;

    while (running) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (!running) break; 
            std::cerr << "Failed to accept client connection.\n";
            continue;
        }

        std::thread client_thread(handle_client, client_socket);
        client_thread.detach();
    }
    close(server_socket);
}

void load_config() {
    std::ifstream config_file("conf.json");
    if (!config_file) {
        std::cerr << "Could not open conf.json.\n";
        exit(1);
    }
    json config;
    config_file >> config;

    host = config.value("host", "127.0.0.1");
    port = config.value("port", 80);
    int buffer_size = config.value("buffer_size", 4096);

    for (const auto& target : config["targets"]) {
        std::string target_str = target;
        size_t colon_pos = target_str.find(':');
        if (colon_pos != std::string::npos) {
            std::string target_host = target_str.substr(0, colon_pos);
            int target_port = std::stoi(target_str.substr(colon_pos + 1));
            targets.emplace_back(target_host, target_port);
        }
    }
}

void monitor_for_exit() {
    char input;
    while (running) {
        std::cin >> input;
        if (input == 'q') {
            running = false;
            std::cout << "Shutting down server..." << std::endl;
        }
    }
}

int main() {
    load_config();
    if (targets.empty()) {
        std::cerr << "No targets specified in conf.json.\n";
        return 1;
    }

    std::thread exit_thread(monitor_for_exit);
    start_server(host, port);
    exit_thread.join(); 
    return 0;
}
