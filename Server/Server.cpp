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
#include <windows.h> // For console color functions
#include <csignal>   // For signal handling

#pragma comment(lib, "ws2_32.lib")

// Helper functions for color output
void SetColor(WORD attributes) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, attributes);
}

void ResetColor() {
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

// Global mapping for client colors
#include <map>
std::mutex color_mutex;
std::map<std::string, WORD> aircraft_colors;
std::vector<WORD> available_colors = {
    FOREGROUND_GREEN | FOREGROUND_INTENSITY,     // Bright Green
    FOREGROUND_RED | FOREGROUND_INTENSITY,       // Bright Red
    FOREGROUND_BLUE | FOREGROUND_INTENSITY,      // Bright Blue
    FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY, // Yellow
    FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY, // Cyan
    FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY   // Magenta
};
size_t next_color_index = 0;

WORD get_color_for_aircraft(const std::string& aircraft_id) {
    std::lock_guard<std::mutex> lock(color_mutex);
    if (aircraft_colors.find(aircraft_id) == aircraft_colors.end()) {
        aircraft_colors[aircraft_id] = available_colors[next_color_index];
        next_color_index = (next_color_index + 1) % available_colors.size();
    }
    return aircraft_colors[aircraft_id];
}

// Structure to maintain running average for fuel statistics
struct FuelStats {
    double sum = 0.0;
    size_t count = 0;
};

std::mutex data_mutex;
std::unordered_map<std::string, FuelStats> fuel_stats;  // Running stats per aircraft
std::unordered_map<std::string, double> final_avg_fuel;   // Final fuel consumption per aircraft
std::vector<std::thread> thread_pool;                     // Store client threads
std::queue<SOCKET> client_queue;                          // Queue to manage client connections
std::mutex queue_mutex;                                   // Mutex for thread-safe queue access

volatile std::sig_atomic_t server_running = 1;

// Signal handler for Ctrl+C
void signal_handler(int signal) {
    if (signal == SIGINT) {
        server_running = 0;
        std::cout << "\n[SERVER] Shutdown signal received. Stopping server gracefully...\n";
    }
}

// Function to calculate fuel consumption (running average)
double calculate_fuel_consumption(const FuelStats& stats) {
    if (stats.count == 0) return 0.0;
    return stats.sum / static_cast<double>(stats.count);
}

// Function to store final average consumption in a text file
void store_final_result(const std::string& aircraft_id, double average) {
    std::ofstream outfile("final_results.txt", std::ios::app);
    if (outfile.is_open()) {
        outfile << "Aircraft: " << aircraft_id << " | Average Fuel Consumption: " << average << "\n";
        outfile.close();
    }
    else {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] Could not open final_results.txt for writing.\n";
        ResetColor();
    }
}

// Function to handle each client in a separate thread with message framing and timeout
void handle_client(SOCKET client_socket) {
    // Set receive timeout to 15 seconds
    int timeout = 15000; // milliseconds
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    const int BUF_SIZE = 1024;
    // Increase buffer size by 1 for null terminator.
    char temp_buffer[BUF_SIZE + 1];
    std::string data_buffer;  // Buffer to store partial data
    int bytes_received;
    std::string aircraft_id;
    bool received_any_valid_data = false;  // Tracks if any valid data was received

    while (server_running) {
        bytes_received = recv(client_socket, temp_buffer, BUF_SIZE, 0);
        if (bytes_received == 0) {
            WORD client_color = aircraft_id.empty() ? (FOREGROUND_RED | FOREGROUND_INTENSITY)
                : get_color_for_aircraft(aircraft_id);
            SetColor(client_color);
            std::cout << "[CLIENT DISCONNECTED] " << aircraft_id << " (Graceful Disconnect)\n";
            ResetColor();
            break;
        }
        else if (bytes_received == SOCKET_ERROR) {
            int error_code = WSAGetLastError();
            if (error_code == WSAETIMEDOUT) {
                WORD client_color = aircraft_id.empty() ? (FOREGROUND_RED | FOREGROUND_INTENSITY)
                    : get_color_for_aircraft(aircraft_id);
                SetColor(client_color);
                std::cout << "[TIMEOUT] No data received for 15 seconds. Finalizing connection for " << aircraft_id << ".\n";
                ResetColor();
                break;
            }
            else {
                SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
                std::cerr << "[ERROR] recv() failed for " << aircraft_id
                    << " with error code: " << error_code << "\n";
                ResetColor();
                break;
            }
        }

        // Null-terminate the received data safely.
        temp_buffer[bytes_received] = '\0';
        data_buffer.append(temp_buffer);

        // Process complete messages delimited by newline
        size_t pos;
        while ((pos = data_buffer.find("\n")) != std::string::npos) {
            std::string message = data_buffer.substr(0, pos);
            data_buffer.erase(0, pos + 1);  // Remove the processed message

            // Parse the message to extract the aircraft ID, timestamp, and fuel
            std::stringstream ss(message);
            std::string received_aircraft_id, timestamp, fuel;
            if (std::getline(ss, received_aircraft_id, ',') &&
                std::getline(ss, timestamp, ',') &&
                std::getline(ss, fuel, ',')) {
                try {
                    double fuel_value = std::stod(fuel);
                    {
                        std::lock_guard<std::mutex> lock(data_mutex);
                        fuel_stats[received_aircraft_id].sum += fuel_value;
                        fuel_stats[received_aircraft_id].count++;
                    }
                    aircraft_id = received_aircraft_id;
                    received_any_valid_data = true;

                    // Use the color for this aircraft when printing the received data.
                    WORD client_color = get_color_for_aircraft(received_aircraft_id);
                    {
                        std::lock_guard<std::mutex> lock(data_mutex);
                        SetColor(client_color);
                        std::cout << "[DATA RECEIVED] " << message << std::endl;
                        ResetColor();
                    }
                }
                catch (const std::exception& e) {
                    SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
                    std::cerr << "[ERROR] Parsing fuel value failed: " << e.what() << "\n";
                    ResetColor();
                }
            }
            else {
                SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
                std::cerr << "[ERROR] Message format incorrect: " << message << "\n";
                ResetColor();
            }
        }
    }

    // After client disconnects or timeout, calculate and store the final average if data was received
    if (received_any_valid_data && !aircraft_id.empty() && fuel_stats[aircraft_id].count > 0) {
        double avg = calculate_fuel_consumption(fuel_stats[aircraft_id]);
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            final_avg_fuel[aircraft_id] = avg;
        }
        WORD client_color = get_color_for_aircraft(aircraft_id);
        SetColor(client_color);
        std::cout << "[INFO] Flight ended for " << aircraft_id
            << ". Average fuel consumption: " << avg << " per time unit.\n";
        ResetColor();

        store_final_result(aircraft_id, avg);
    }

    closesocket(client_socket);
}

// Thread worker function (handles clients from the queue)
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
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }
        handle_client(client_socket);
    }
}

int main() {
    // Install the signal handler for Ctrl+C
    std::signal(SIGINT, signal_handler);

    WSADATA wsa;
    SOCKET server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    int client_len = sizeof(client_addr);

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] WSAStartup failed.\n";
        ResetColor();
        return 1;
    }

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] Failed to create socket.\n";
        ResetColor();
        WSACleanup();
        return 1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(5000);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] Bind failed.\n";
        ResetColor();
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    if (listen(server_socket, 10) == SOCKET_ERROR) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] Listen failed.\n";
        ResetColor();
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "[SERVER] Listening on port 5000...\n";
    ResetColor();

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
                SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
                std::cerr << "[ERROR] Failed to accept connection.\n";
                ResetColor();
                continue;
            }
            SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::cout << "[NEW CLIENT] Connected.\n";
            ResetColor();
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                client_queue.push(client_socket);
            }
        }
    }

    for (auto& t : thread_pool) {
        if (t.joinable()) t.join();
    }

    closesocket(server_socket);
    WSACleanup();
    return 0;
}
