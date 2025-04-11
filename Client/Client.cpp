#include <iostream>
#include <fstream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <chrono>
#include <vector>
#include <filesystem>
#include <sstream>
#include <windows.h>     // Header for console color functions
#include <random>
#include <string>
#include <ctime>
#include <chrono>
#include <process.h>      // For _getpid()

#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;

// This is teh helper functions for console color

void SetColor(WORD attributes) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, attributes);
}
void ResetColor() {
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

// This generates a unique aircraft ID and combines a high-resolution clock, process ID, and a random 4-digit number.
std::string generate_aircraft_id() {
    auto now = std::chrono::high_resolution_clock::now();
    auto microsec = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    int pid = _getpid();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1000, 9999);
    return "Plane_" + std::to_string(microsec) + "" + std::to_string(pid) + "" + std::to_string(dist(gen));
}

// Find available telemetry files in the current directory
std::vector<std::string> find_telemetry_files() {
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(fs::current_path()))
    {
        if (entry.path().extension() == ".txt")
        {
            files.push_back(entry.path().filename().string());
        }
    }
    return files;
}

// Trim leading spaces
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" ");
    if (first == std::string::npos) return str;
    return str.substr(first);
}


// Check for shutdown signal: returns true if shutdown.txt exists.

bool shutdown_requested(const fs::path& shutdownFile) {
    return fs::exists(shutdownFile);
}

// Function to send telemetry data with graceful shutdown checks

void send_telemetry(const std::string& server_ip, const std::string& file_path, const std::string& aircraft_id) {
    WSADATA wsa;
    SOCKET client_socket;
    struct sockaddr_in server_addr;

    // Initialize Winsock.
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] WSAStartup failed.\n";
        ResetColor();
        return;
    }

    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == INVALID_SOCKET) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] Failed to create socket.\n";
        ResetColor();
        WSACleanup();
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5000);
    if (InetPtonA(AF_INET, server_ip.c_str(), &server_addr.sin_addr) != 1) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] Invalid IP address format.\n";
        ResetColor();
        closesocket(client_socket);
        WSACleanup();
        return;
    }
    if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] Could not connect to server.\n";
        ResetColor();
        closesocket(client_socket);
        WSACleanup();
        return;
    }

    SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "[CLIENT] Connected to server as " << aircraft_id << ".\n";
    ResetColor();

    std::cout << "[CLIENT] Current directory: " << fs::current_path() << "\n";

    std::ifstream file(file_path);
    if (!file) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] File not found: " << file_path << "\n";
        ResetColor();
        closesocket(client_socket);
        WSACleanup();
        return;
    }

    // Build shutdown file path.
    fs::path shutdownFile = fs::current_path() / "shutdown.txt";

    std::string line;
    bool firstLine = true;
    while (std::getline(file, line)) {
        if (shutdown_requested(shutdownFile)) {
            SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
            std::cout << "[CLIENT] Shutdown command received. Exiting gracefully.\n";
            ResetColor();
            break;
        }
        line = trim(line);
        if (line.empty())
            continue;
        if (firstLine) { // Skip header if present.
            firstLine = false;
            continue;
        }
        std::stringstream ss(line);
        std::string timestamp, fuel_remaining;
        if (std::getline(ss, timestamp, ',') && std::getline(ss, fuel_remaining, ',')) {
            std::string data_to_send = aircraft_id + "," + timestamp + "," + fuel_remaining + "\n";
            int result = send(client_socket, data_to_send.c_str(), static_cast<int>(data_to_send.length()), 0);
            if (result == SOCKET_ERROR) {
                SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
                std::cerr << "[ERROR] Failed to send data. Server might be down.\n";
                ResetColor();
                break;
            }
            SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::cout << "[SENT] " << data_to_send;
            ResetColor();
            // Sleep in 10 segments (each 100ms) to check for shutdown.
            for (int i = 0; i < 10; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (shutdown_requested(shutdownFile)) {
                    SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
                    std::cout << "[CLIENT] Shutdown command received during sleep. Exiting gracefully.\n";
                    ResetColor();
                    goto exit_loop;
                }
            }
        }
    }
exit_loop:
    shutdown(client_socket, SD_BOTH);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    closesocket(client_socket);
    WSACleanup();
}

int main() {
    // Here we set the ip odf the server.
    std::string server_ip = "10.144.122.167";
    // Generate a unique aircraft ID.
    std::string aircraft_id = generate_aircraft_id();

    std::vector<std::string> telemetry_files = find_telemetry_files();
    if (telemetry_files.empty()) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] No telemetry files found.\n";
        ResetColor();
        return 1;
    }
    // Select one telemetry file (randomly or fixed).
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, static_cast<int>(telemetry_files.size()) - 1);
    int choice = dist(gen);

    SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "[CLIENT] Selected file: " << telemetry_files[choice] << "\n";
    ResetColor();

    send_telemetry(server_ip, telemetry_files[choice], aircraft_id);
    return 0;
}

