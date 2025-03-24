#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <queue>
#include <fstream>

#pragma comment(lib, "ws2_32.lib")

std::mutex data_mutex;
std::unordered_map<std::string, std::vector<double>> fuel_data; // Fuel history per aircraft
std::unordered_map<std::string, double> final_avg_fuel; // Final fuel consumption per aircraft
std::vector<std::thread> thread_pool;  // Store client threads
std::queue<SOCKET> client_queue;  // Queue to manage client connections
std::mutex queue_mutex;  // Mutex for thread-safe queue access

bool server_running = true;

// Function to calculate fuel consumption
double calculate_fuel_consumption(const std::vector<double>& fuel_levels) 
{
    if (fuel_levels.size() < 2) return 0.0; // Not enough data to calculate consumption
    double total_consumption = fuel_levels.front() - fuel_levels.back();
    return total_consumption / (fuel_levels.size() - 1);
}

// Function to handle each client in a separate thread
void handle_client(SOCKET client_socket) {
    char buffer[1024];
    int bytes_received;
    std::string aircraft_id;

    while (true) {
        bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);

        if (bytes_received == 0) {
            std::cout << "[CLIENT DISCONNECTED] " << aircraft_id << std::endl;
            break;
        }
        else if (bytes_received == SOCKET_ERROR) {
            // Unexpected error (client crash, force quit, network issue)
            std::cerr << "[ERROR] recv() failed for " << aircraft_id << std::endl;
            break;
        }

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

    // Only calculate fuel consumption if the aircraft sent data
    if (!aircraft_id.empty() && !fuel_data[aircraft_id].empty()) {
        final_avg_fuel[aircraft_id] = calculate_fuel_consumption(fuel_data[aircraft_id]);
        std::cout << "[INFO] Flight ended for " << aircraft_id
            << ". Average fuel consumption: " << final_avg_fuel[aircraft_id] << " per time unit.\n";
    }

    closesocket(client_socket);
}

// Thread worker function (Handles clients from queue)
void client_worker() 
{
    while (server_running) 
    {
        SOCKET client_socket;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (!client_queue.empty()) 
            {
                client_socket = client_queue.front();
                client_queue.pop();
            }
            else 
            {
                continue;
            }
        }
        handle_client(client_socket);
    }
}

int main() 
{
    WSADATA wsa;
    SOCKET server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    int client_len = sizeof(client_addr);

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) 
    {
        std::cerr << "[ERROR] WSAStartup failed.\n";
        return 1;
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) 
    {
        std::cerr << "[ERROR] Failed to create socket.\n";
        WSACleanup();
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(5000);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) 
    {
        std::cerr << "[ERROR] Bind failed.\n";
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    if (listen(server_socket, 10) == SOCKET_ERROR) 
    {
        std::cerr << "[ERROR] Listen failed.\n";
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    std::cout << "[SERVER] Listening on port 5000...\n";

    // Create a fixed-size thread pool (5 worker threads for handling clients)
    for (int i = 0; i < 5; ++i) 
    {
        thread_pool.emplace_back(client_worker);
    }

    fd_set master_set, read_fds;
    FD_ZERO(&master_set);
    FD_SET(server_socket, &master_set);

    while (server_running) 
    {
        read_fds = master_set;
        struct timeval timeout = { 1, 0 };  // 1-second timeout for checking new connections
        int activity = select(0, &read_fds, nullptr, nullptr, &timeout);

        if (activity > 0 && FD_ISSET(server_socket, &read_fds)) 
        {
            client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
            if (client_socket == INVALID_SOCKET) 
            {
                std::cerr << "[ERROR] Failed to accept connection.\n";
                continue;
            }

            std::cout << "[NEW CLIENT] Connected.\n";

            // Add client to queue for worker threads to handle
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                client_queue.push(client_socket);
            }
        }
    }

    // Clean up worker threads
    for (auto& t : thread_pool) 
    {
        if (t.joinable()) t.join();
    }

    closesocket(server_socket);
    WSACleanup();
    return 0;
}
