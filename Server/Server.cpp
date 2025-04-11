#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <queue>
#include <set>
#include <fstream>
#include <string>
#include <chrono>
#include <windows.h>     // For console color functions
#include <csignal>       // For signal handling
#include <condition_variable>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")

// ------------------------------
// Minimal Logging Helpers
// ------------------------------
void SetColor(WORD attributes) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, attributes);
}
void ResetColor() {
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

// ------------------------------
// Global Variables & Data Structures
// ------------------------------
#include <map>
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

// Structure to keep fuel consumption stats.
struct FuelStats {
    double sum = 0.0;
    size_t count = 0;
};

std::mutex data_mutex;
std::unordered_map<std::string, FuelStats> fuel_stats;  // Running stats per aircraft
std::unordered_map<std::string, double> final_avg_fuel;   // Final averages

// Global vector for incoming messages .
// In this version, we store each message only once.
std::vector<std::string> all_messages;
std::mutex messages_mutex;

// Queue for client sockets and worker threads.
std::vector<std::thread> thread_pool;
std::queue<SOCKET> client_queue;
std::mutex queue_mutex;
std::condition_variable cv;

// Global flag to indicate server is running.
volatile std::sig_atomic_t server_running = 1;

// Tracks number of currently active clients in a thread-safe manner for idle detection and memory cleanup
std::atomic<int> active_clients(0);

// Timestamp to track the last network activity.
std::chrono::steady_clock::time_point lastActivity;

// ------------------------------
// Memory Monitor Thread Function
// Checks every second if all_messages exceeds a threshold and flushes it.
void memory_monitor() {
    const size_t MAX_BUFFER_SIZE = 10000;  
    while (server_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::lock_guard<std::mutex> lock(messages_mutex);
        if (all_messages.size() > MAX_BUFFER_SIZE) {
            all_messages.clear();
            all_messages.shrink_to_fit();
            std::cout << "[MEMORY MONITOR] Buffer exceeded threshold; flushing messages.\n";
        }
    }
}

// ------------------------------
// Signal Handler
// ------------------------------
void signal_handler(int signal) {
    if (signal == SIGINT) {
        server_running = 0;
        std::cout << "\n[SERVER] Shutdown signal received. Stopping server gracefully...\n";
        cv.notify_all();  // Wake up worker threads.
    }
}

// ------------------------------
// Processing Functions
// ------------------------------
double calculate_fuel_consumption(const FuelStats& stats) {
    if (stats.count == 0)
        return 0.0;
    return stats.sum / static_cast<double>(stats.count);
}

void store_all_final_results() {
    std::ofstream outfile("final_results.txt", std::ios::app);
    if (!outfile.is_open()) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] Could not open final_results.txt for writing.\n";
        ResetColor();
        return;
    }

    std::lock_guard<std::mutex> lock(data_mutex); // protect access to final_avg_fuel

    for (const auto& [aircraft_id, avg] : final_avg_fuel) {
        outfile << "Aircraft: " << aircraft_id
            << " | Average Fuel Consumption: " << avg << "\n";
    }

    outfile.close();
}


// Process a complete message. Minimal logging.
void process_message(const std::string& message, std::string& aircraft_id, bool& received_any_valid_data) {
    {
        std::lock_guard<std::mutex> lock(messages_mutex);
        all_messages.push_back(message);
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
        }
        catch (const std::exception& e) {
            (void)e; // Suppress unreferenced variable warning.
        }

    }
    if (received_aircraft_id.empty() || timestamp.empty() || fuel.empty())
        return; 

}

// ------------------------------
// Client Handling Function
// ------------------------------
void handle_client(SOCKET client_socket) {
    // Set a 30-second receive timeout.
    int timeout = 30000;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    const int BUF_SIZE = 1024;
    char temp_buffer[BUF_SIZE + 1];
    std::string data_buffer;
    int bytes_received;
    std::string aircraft_id;
    bool received_any_valid_data = false;

    while (server_running) {
        bytes_received = recv(client_socket, temp_buffer, BUF_SIZE, 0);
        if (bytes_received == 0) {
            break; // Client disconnected gracefully.
        }
        else if (bytes_received == SOCKET_ERROR) {
            int error_code = WSAGetLastError();
            if (error_code == WSAETIMEDOUT)
                break;
            else
                break;
        }
        temp_buffer[bytes_received] = '\0';
        data_buffer.append(temp_buffer);
        lastActivity = std::chrono::steady_clock::now();

        // Process complete messages .
        size_t pos;
        while ((pos = data_buffer.find("\n")) != std::string::npos) {
            std::string message = data_buffer.substr(0, pos);
            data_buffer.erase(0, pos + 1);
            process_message(message, aircraft_id, received_any_valid_data);
        }
    }
    // Flush any leftover data.
    if (!data_buffer.empty()) {
        data_buffer.push_back('\n');
        size_t pos;
        while ((pos = data_buffer.find("\n")) != std::string::npos) {
            std::string message = data_buffer.substr(0, pos);
            data_buffer.erase(0, pos + 1);
            process_message(message, aircraft_id, received_any_valid_data);
        }
    }
    // Finalize if valid data was received, calculate and store final average.
    if (received_any_valid_data && !aircraft_id.empty() && fuel_stats[aircraft_id].count > 0) {
        double avg = calculate_fuel_consumption(fuel_stats[aircraft_id]);
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            final_avg_fuel[aircraft_id] = avg;
        }
    }
    else {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[WARNING] No valid data received from " << aircraft_id << "\n";
        ResetColor();
    }

    closesocket(client_socket);
    active_clients.fetch_sub(1);  // Decrement active client count.
}

// ------------------------------
// Worker Thread Function
// ------------------------------
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

// ------------------------------
// Main Function
// ------------------------------
int main() {
    std::signal(SIGINT, signal_handler);
    lastActivity = std::chrono::steady_clock::now();

    WSADATA wsa;
    SOCKET server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    int client_len = sizeof(client_addr);

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

    // Minimal logging: announce server is listening.
    SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "[SERVER] Listening on port 5000...\n";
    ResetColor();

    // Start worker threads.
    for (int i = 0; i < 5; ++i) {
        thread_pool.emplace_back(client_worker);
    }

    // Start memory monitor thread.
    std::thread memMonitor(memory_monitor);

    active_clients.store(0);

    fd_set master_set, read_fds;
    FD_ZERO(&master_set);
    FD_SET(server_socket, &master_set);

    // Main loop: accept new clients.
    while (server_running) {
        read_fds = master_set;
        struct timeval timeout_val = { 1, 0 };  // 1-second select timeout.
        int activity = select(0, &read_fds, nullptr, nullptr, &timeout_val);
        if (activity > 0 && FD_ISSET(server_socket, &read_fds)) {
            client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
            if (client_socket == INVALID_SOCKET)
                continue;
            SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::cout << "[NEW CLIENT] Connected.\n";
            ResetColor();
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                client_queue.push(client_socket);
            }
            cv.notify_one();
            lastActivity = std::chrono::steady_clock::now();
            active_clients.fetch_add(1);
        }
        // When there are no active clients and no activity for 30 seconds, it will flush extra memory.
        auto now = std::chrono::steady_clock::now();
        if (active_clients.load() == 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - lastActivity).count() >= 30) {
            std::lock_guard<std::mutex> lock(messages_mutex);
            if (!all_messages.empty()) {
                all_messages.clear();
                all_messages.shrink_to_fit();
                std::cout << "[INFO] All clients ended; memory flushed.\n";
            }
            lastActivity = now;
        }
    }

    cv.notify_all();
    for (auto& t : thread_pool) {
        if (t.joinable())
            t.join();
    }

    server_running = 0;
    if (memMonitor.joinable())
        memMonitor.join();

    {
        std::lock_guard<std::mutex> lock(messages_mutex);
        all_messages.clear();
        all_messages.shrink_to_fit();
    }

    // Final buffered write of all results
    store_all_final_results();

    closesocket(server_socket);
    WSACleanup();
    return 0;
}