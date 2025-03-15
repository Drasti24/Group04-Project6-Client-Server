#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <queue>

#pragma comment(lib, "ws2_32.lib")

std::mutex data_mutex;
std::unordered_map<std::string, std::vector<double>> fuel_data; // Fuel history per aircraft
std::unordered_map<std::string, double> final_avg_fuel; // Final fuel consumption per aircraft
std::vector<std::thread> thread_pool;  // Store client threads
std::queue<SOCKET> client_queue;  // Queue to manage client connections
std::mutex queue_mutex;  // Mutex for thread-safe queue access

bool server_running = true;

// Function to calculate fuel consumption
double calculate_fuel_consumption(const std::vector<double>& fuel_levels) {
    if (fuel_levels.size() < 2) return 0.0; // Not enough data to calculate consumption
    double total_consumption = fuel_levels.front() - fuel_levels.back();
    return total_consumption / (fuel_levels.size() - 1);
}

// Function to handle each client in a separate thread
void handle_client(SOCKET client_socket) {
    char buffer[1024];
    int bytes_received;
    std::string aircraft_id;

    while ((bytes_received = recv(client_socket, buffer, sizeof(buffer), 0)) > 0) {
        buffer[bytes_received] = '\0';  // Null-terminate received data

        std::lock_guard<std::mutex> lock(data_mutex);
        std::cout << "[DATA RECEIVED] " << buffer << std::endl;

        std::stringstream ss(buffer);
        std::string received_aircraft_id, timestamp, fuel;

        if (std::getline(ss, received_aircraft_id, ',') &&
            std::getline(ss, timestamp, ',') &&
            std::getline(ss, fuel, ',')) {

            double fuel_remaining = std::stod(fuel);
            fuel_data[received_aircraft_id].push_back(fuel_remaining);
            aircraft_id = received_aircraft_id;
        }
    }

    if (!aircraft_id.empty() && !fuel_data[aircraft_id].empty()) {
        final_avg_fuel[aircraft_id] = calculate_fuel_consumption(fuel_data[aircraft_id]);
        std::cout << "[INFO] Flight ended for " << aircraft_id
            << ". Average fuel consumption: " << final_avg_fuel[aircraft_id] << " per time unit.\n";
    }

    closesocket(client_socket);
    std::cout << "[CLIENT DISCONNECTED]" << std::endl;
}

// Thread worker function (Manages client queue)
void client_worker() {
    while (server_running) {
        SOCKET client_socket;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (!client_queue.empty()) {
                client_socket = client_queue.front();
                client_queue.pop();
            }
            else {
                continue;
            }
        }

        handle_client(client_socket);
    }
}

int main() {
    WSADATA wsa;
    SOCKET server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    int client_len = sizeof(client_addr);

    WSAStartup(MAKEWORD(2, 2), &wsa);
    server_socket = socket(AF_INET, SOCK_STREAM, 0);

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(5000);

    bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr));
    listen(server_socket, 10);

    std::cout << "[SERVER] Listening on port 5000...\n";

    // Create a fixed-size thread pool (5 worker threads for handling clients)
    for (int i = 0; i < 5; ++i) {
        thread_pool.emplace_back(client_worker);
    }

    while (server_running) {
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket == INVALID_SOCKET) continue;

        std::cout << "[NEW CLIENT] Connected.\n";

        // Add client to queue for thread workers to handle
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            client_queue.push(client_socket);
        }
    }

    // Clean up worker threads
    for (auto& t : thread_pool) {
        if (t.joinable()) t.join();
    }

    closesocket(server_socket);
    WSACleanup();
    return 0;
}
