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
#include <string>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

std::mutex data_mutex;
std::unordered_map<std::string, std::vector<double>> fuel_data; // Fuel history per aircraft
std::unordered_map<std::string, double> final_avg_fuel; // Final fuel consumption per aircraft
std::vector<std::thread> thread_pool;  // Store client threads
std::queue<SOCKET> client_queue;       // Queue to manage client connections
std::mutex queue_mutex;                // Mutex for thread-safe queue access

bool server_running = true;

// Function to calculate fuel consumption
double calculate_fuel_consumption(const std::vector<double>& fuel_levels)
{
    if (fuel_levels.size() < 2) return 0.0; // Not enough data to calculate consumption
    double total_consumption = fuel_levels.front() - fuel_levels.back();
    return total_consumption / (fuel_levels.size() - 1);
}

// Function to handle each client in a separate thread with message framing
void handle_client(SOCKET client_socket) {
    const int BUF_SIZE = 1024;
    char temp_buffer[BUF_SIZE];
    std::string data_buffer;  // Buffer to store partial data
    int bytes_received;
    std::string aircraft_id;
    bool received_any_valid_data = false;  // Tracks if any valid data was received

    while (true) {
        bytes_received = recv(client_socket, temp_buffer, BUF_SIZE, 0);
        if (bytes_received == 0) {
            std::cout << "[CLIENT DISCONNECTED] " << aircraft_id << " (Graceful Disconnect)\n";
            break;
        }
        else if (bytes_received == SOCKET_ERROR) {
            int error_code = WSAGetLastError();
            std::cerr << "[ERROR] recv() failed for " << aircraft_id
                << " with error code: " << error_code << "\n";
            break;
        }

        // Append the received data to our buffer and null-terminate
        temp_buffer[bytes_received] = '\0';
        data_buffer.append(temp_buffer);

        // Process complete messages delimited by newline
        size_t pos;
        while ((pos = data_buffer.find("\n")) != std::string::npos) {
            std::string message = data_buffer.substr(0, pos);
            data_buffer.erase(0, pos + 1);  // Remove the processed message

            {
                std::lock_guard<std::mutex> lock(data_mutex);
                std::cout << "[DATA RECEIVED] " << message << std::endl;
            }

            std::stringstream ss(message);
            std::string received_aircraft_id, timestamp, fuel;

            if (std::getline(ss, received_aircraft_id, ',') &&
                std::getline(ss, timestamp, ',') &&
                std::getline(ss, fuel, ',')) {

                try {
                    double fuel_remaining = std::stod(fuel);
                    {
                        std::lock_guard<std::mutex> lock(data_mutex);
                        fuel_data[received_aircraft_id].push_back(fuel_remaining);
                    }
                    aircraft_id = received_aircraft_id;
                    received_any_valid_data = true;  // Mark that we received data
                }
                catch (const std::exception& e) {
                    std::cerr << "[ERROR] Parsing fuel value failed: " << e.what() << "\n";
                }
            }
            else {
                std::cerr << "[ERROR] Message format incorrect: " << message << "\n";
            }
        }
    }

    // After client disconnects, calculate average if data was received
    if (received_any_valid_data && !aircraft_id.empty() && !fuel_data[aircraft_id].empty()) {
        double avg = calculate_fuel_consumption(fuel_data[aircraft_id]);
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            final_avg_fuel[aircraft_id] = avg;
        }
        std::cout << "[INFO] Flight ended for " << aircraft_id
            << ". Average fuel consumption: " << avg << " per time unit.\n";
    }

    closesocket(client_socket);
}

// Thread worker function (handles clients from the queue)
void client_worker()
{
    while (server_running) {
        SOCKET client_socket;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (!client_queue.empty()) {
                client_socket = client_queue.front();
                client_queue.pop();
            }
            else {
                // Sleep briefly to avoid busy waiting when the queue is empty.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[ERROR] WSAStartup failed.\n";
        return 1;
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        std::cerr << "[ERROR] Failed to create socket.\n";
        WSACleanup();
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(5000);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "[ERROR] Bind failed.\n";
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    if (listen(server_socket, 10) == SOCKET_ERROR) {
        std::cerr << "[ERROR] Listen failed.\n";
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    std::cout << "[SERVER] Listening on port 5000...\n";

    // Create a fixed-size thread pool (5 worker threads)
    for (int i = 0; i < 5; ++i) {
        thread_pool.emplace_back(client_worker);
    }

    fd_set master_set, read_fds;
    FD_ZERO(&master_set);
    FD_SET(server_socket, &master_set);

    while (server_running) {
        read_fds = master_set;
        struct timeval timeout = { 1, 0 };  // 1-second timeout for checking new connections
        int activity = select(0, &read_fds, nullptr, nullptr, &timeout);

        if (activity > 0 && FD_ISSET(server_socket, &read_fds)) {
            client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
            if (client_socket == INVALID_SOCKET) {
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
    for (auto& t : thread_pool) {
        if (t.joinable()) t.join();
    }

    closesocket(server_socket);
    WSACleanup();
    return 0;
}
