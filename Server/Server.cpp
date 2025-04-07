#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <queue>
#include <fstream>
#include <string>
#include <string_view>
#include <chrono>
#include <csignal>
#include <map>
#include <filesystem>
#include <windows.h>
#include <condition_variable>

#pragma comment(lib, "ws2_32.lib")

// Uncomment to enable result logging to console
// #define DEBUG_LOG

// Console color helpers
void SetColor(WORD attributes) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, attributes);
}
void ResetColor() {
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

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

struct FuelStats {
    double sum = 0.0;
    size_t count = 0;
};

std::mutex data_mutex;
std::unordered_map<std::string, FuelStats> fuel_stats;
std::unordered_map<std::string, double> final_avg_fuel;

std::vector<std::thread> thread_pool;
std::queue<SOCKET> client_queue;
std::mutex queue_mutex;
std::condition_variable client_cv;
volatile std::sig_atomic_t server_running = 1;

std::queue<std::string> result_queue;
std::mutex file_mutex;
bool writer_running = true;

void signal_handler(int signal) {
    if (signal == SIGINT) {
        server_running = 0;
        client_cv.notify_all();
        std::cout << "\n[SERVER] Shutdown signal received. Stopping server gracefully...\n";
    }
}

double calculate_fuel_consumption(const FuelStats& stats) {
    if (stats.count == 0) return 0.0;
    return stats.sum / static_cast<double>(stats.count);
}

void file_writer_thread() {
    std::ofstream outfile("final_results.txt", std::ios::app);
    if (!outfile.is_open()) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] Could not open final_results.txt.\n";
        ResetColor();
        return;
    }

    while (writer_running || !result_queue.empty()) {
        std::string line;
        {
            std::lock_guard<std::mutex> lock(file_mutex);
            if (!result_queue.empty()) {
                line = result_queue.front();
                result_queue.pop();
            }
        }

        if (!line.empty()) {
            outfile << line << "\n";
            outfile.flush();
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    outfile.close();
}

void handle_client(SOCKET client_socket) {
    int timeout = 15000;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    const int BUF_SIZE = 1024;
    char temp_buffer[BUF_SIZE + 1];
    std::string data_buffer;
    int bytes_received;
    std::string aircraft_id;
    bool received_any_valid_data = false;
    WORD cached_color = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    int msg_counter = 0;

    while (server_running) {
        bytes_received = recv(client_socket, temp_buffer, BUF_SIZE, 0);
        if (bytes_received <= 0) {
            if (bytes_received == 0 || WSAGetLastError() == WSAETIMEDOUT) {
                std::string reason = (bytes_received == 0) ? "Graceful Disconnect" : "Timeout";
#ifdef DEBUG_LOG
                SetColor(cached_color);
                std::cout << "[DISCONNECT] " << aircraft_id << ": " << reason << "\n";
                ResetColor();
#endif
            }
            break;
        }

        temp_buffer[bytes_received] = '\0';
        data_buffer.append(temp_buffer);

        size_t pos;
        while ((pos = data_buffer.find("\n")) != std::string::npos) {
            std::string_view message(data_buffer.data(), pos);
            data_buffer.erase(0, pos + 1);

            size_t pos1 = message.find(',');
            size_t pos2 = message.find(',', pos1 + 1);
            if (pos1 != std::string::npos && pos2 != std::string::npos) {
                std::string_view id = message.substr(0, pos1);
                std::string_view timestamp = message.substr(pos1 + 1, pos2 - pos1 - 1);
                std::string_view fuel = message.substr(pos2 + 1);

                try {
                    double fuel_value = std::stod(std::string(fuel));
                    {
                        std::lock_guard<std::mutex> lock(data_mutex);
                        fuel_stats[std::string(id)].sum += fuel_value;
                        fuel_stats[std::string(id)].count++;
                    }

                    aircraft_id = std::string(id);
                    if (msg_counter == 0)
                        cached_color = get_color_for_aircraft(aircraft_id);

                    ++msg_counter;
                    received_any_valid_data = true;
                }
                catch (...) {
                    SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
                    std::cerr << "[ERROR] Invalid fuel value: " << fuel << "\n";
                    ResetColor();
                }
            }
        }
    }

    if (received_any_valid_data && !aircraft_id.empty() && fuel_stats[aircraft_id].count > 0) {
        double avg = calculate_fuel_consumption(fuel_stats[aircraft_id]);
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            final_avg_fuel[aircraft_id] = avg;
        }

#ifdef DEBUG_LOG
        SetColor(cached_color);
        std::cout << "[RESULT] " << aircraft_id << " avg fuel consumption: " << avg << "\n";
        ResetColor();
#endif

        std::ostringstream oss;
        oss << "Aircraft: " << aircraft_id << " | Average Fuel Consumption: " << avg;
        std::lock_guard<std::mutex> lock(file_mutex);
        result_queue.push(oss.str());
    }

    closesocket(client_socket);
}

void client_worker() {
    while (server_running) {
        SOCKET client_socket = INVALID_SOCKET;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            client_cv.wait(lock, [] { return !client_queue.empty() || !server_running; });

            if (!server_running) break;

            client_socket = client_queue.front();
            client_queue.pop();
        }

        if (client_socket != INVALID_SOCKET) {
            handle_client(client_socket);
        }
    }
}

int main() {
    std::signal(SIGINT, signal_handler);

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

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR ||
        listen(server_socket, 10) == SOCKET_ERROR) {
        std::cerr << "[ERROR] Server bind/listen failed.\n";
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    std::cout << "[SERVER] Listening on port 5000...\n";

    for (int i = 0; i < 5; ++i)
        thread_pool.emplace_back(client_worker);

    std::thread writer_thread(file_writer_thread);

    fd_set master_set, read_fds;
    FD_ZERO(&master_set);
    FD_SET(server_socket, &master_set);

    while (server_running) {
        read_fds = master_set;
        struct timeval timeout = { 1, 0 };
        int activity = select(0, &read_fds, nullptr, nullptr, &timeout);

        if (activity > 0 && FD_ISSET(server_socket, &read_fds)) {
            client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
            if (client_socket != INVALID_SOCKET) {
                std::cout << "[NEW CLIENT] Connection accepted.\n";
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    client_queue.push(client_socket);
                }
                client_cv.notify_one();
            }
        }
    }

    for (auto& t : thread_pool) if (t.joinable()) t.join();
    writer_running = false;
    if (writer_thread.joinable()) writer_thread.join();

    std::filesystem::remove("shutdown.txt");
    closesocket(server_socket);
    WSACleanup();
    std::cout << "[SERVER] Shutdown complete.\n";
    return 0;
}