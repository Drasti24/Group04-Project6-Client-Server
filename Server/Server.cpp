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
#include <windows.h>   // For console color functions
#include <csignal>     // For signal handling
#include <condition_variable>
#include <map>

#pragma comment(lib, "ws2_32.lib")

// Our Helper functions for console color

void SetColor(WORD attributes) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, attributes);
}
void ResetColor() {
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

// Global variables and data structures

std::mutex color_mutex;
std::map<std::string, WORD> aircraft_colors;
std::vector<WORD> available_colors = {
    FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    FOREGROUND_RED | FOREGROUND_INTENSITY,
    FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY,
    FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY
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

// This is teh structure to maintain running fuel statistics.
struct FuelStats {
    double sum = 0.0;
    size_t count = 0;
};

std::mutex data_mutex;
std::unordered_map<std::string, FuelStats> fuel_stats;  // Running the stats per aircraft
std::unordered_map<std::string, double> final_avg_fuel;   // Final fuel consumption per aircraft

// Global vector to accumulate messages (this simulates heavy memory usage: store each message 10 times).
std::vector<std::string> all_messages;
std::mutex messages_mutex;

// Thread pool and client connection queue.
std::vector<std::thread> thread_pool;
std::queue<SOCKET> client_queue;
std::mutex queue_mutex;
std::condition_variable cv;

volatile std::sig_atomic_t server_running = 1;  // Global flag for server running

// Signal Handler (Ctrl+C is used for shutting down here)
void signal_handler(int signal) {
    if (signal == SIGINT) {
        server_running = 0;
        std::cout << "\n[SERVER] Shutdown signal received. Stopping server gracefully...\n";
        cv.notify_all();
    }
}


// Function to calculate running average fuel consumption
double calculate_fuel_consumption(const FuelStats& stats) {
    if (stats.count == 0) return 0.0;
    return stats.sum / static_cast<double>(stats.count);
}

// Function to store the final average in a text file
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

// Process a complete message received from a client. This function saves multiple copies of the message (to simulate degraded memory usage),parses the message (expected format: aircraftID,timestamp,fuel_remaining),and updates the running fuel statistics.
void process_message(const std::string& message, std::string& aircraft_id, bool& received_any_valid_data) {
    // Save each message 10 times to simulate heavy memory usage.
    {
        std::lock_guard<std::mutex> lock(messages_mutex);
        for (int i = 0; i < 10; ++i) {
            all_messages.push_back(message);
        }
    }

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
            // Print received data in the assigned color.
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

// Function to handle an individual client connection. It reads data from the socket, processes complete messages delimited by newline, and when the client disconnects, it calculates the final average and stores it.
void handle_client(SOCKET client_socket) {
    int timeout = 15000; // 15-second timeout
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    const int BUF_SIZE = 1024;
    char temp_buffer[BUF_SIZE + 1];
    std::string data_buffer;
    int bytes_received;
    std::string aircraft_id;
    bool received_any_valid_data = false;

    while (server_running) {
        bytes_received = recv(client_socket, temp_buffer, BUF_SIZE, 0);
        if (bytes_received == 0) 
        {
            // Client disconnected gracefully.
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
                std::cerr << "[ERROR] recv() failed for " << aircraft_id << " with error code: " << error_code << "\n";
                ResetColor();
                break;
            }
        }

        temp_buffer[bytes_received] = '\0';
        data_buffer.append(temp_buffer);

        // Process complete messages (delimited by newline)
        size_t pos;
        while ((pos = data_buffer.find("\n")) != std::string::npos) {
            std::string message = data_buffer.substr(0, pos);
            data_buffer.erase(0, pos + 1);
            process_message(message, aircraft_id, received_any_valid_data);
        }
    }

    // Flush remaining data in the buffer.

    if (!data_buffer.empty()) {
        data_buffer.push_back('\n');
        size_t pos;
        while ((pos = data_buffer.find("\n")) != std::string::npos) {
            std::string message = data_buffer.substr(0, pos);
            data_buffer.erase(0, pos + 1);
            process_message(message, aircraft_id, received_any_valid_data);
        }
    }

    // On client disconnect, if any valid data was received, compute and store final average.

    if (received_any_valid_data && !aircraft_id.empty() && fuel_stats[aircraft_id].count > 0) {
        double avg = calculate_fuel_consumption(fuel_stats[aircraft_id]);
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            final_avg_fuel[aircraft_id] = avg;
        }
        SetColor(get_color_for_aircraft(aircraft_id));
        std::cout << "[INFO] Flight ended for " << aircraft_id << ". Average fuel consumption: "
            << avg << "\n";
        ResetColor();
        store_final_result(aircraft_id, avg);
    }

    closesocket(client_socket);
}

// Worker thread function: retrieves clients from the queue and processes them.

void client_worker() {
    while (server_running) {
        SOCKET client_socket = INVALID_SOCKET;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            cv.wait(lock, [] { return !client_queue.empty() || !server_running; });
            if (!client_queue.empty()) {
                client_socket = client_queue.front();
                client_queue.pop();
            }
            else {
                break;
            }
        }
        handle_client(client_socket);
    }
}

// Main Function: sets up networking, starts worker threads, and accepts connections.
int main() {
    std::signal(SIGINT, signal_handler);

    WSADATA wsa;
    SOCKET server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    int client_len = sizeof(client_addr);

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

    // server uses a backlog 
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

    // Start a fixed number (5) of worker threads.
    for (int i = 0; i < 5; ++i) {
        thread_pool.emplace_back(client_worker);
    }

    // Main loop: accept new client connections.
    fd_set master_set, read_fds;
    FD_ZERO(&master_set);
    FD_SET(server_socket, &master_set);

    while (server_running) {
        read_fds = master_set;
        struct timeval timeout = { 1, 0 };  // 1-second timeout
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
            cv.notify_one();
        }
    }

    cv.notify_all();
    for (auto& t : thread_pool) {
        if (t.joinable())
            t.join();
    }

    closesocket(server_socket);
    WSACleanup();
    return 0;
}






