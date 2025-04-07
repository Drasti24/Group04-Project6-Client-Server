#include <iostream>
#include <fstream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <chrono>
#include <vector>
#include <filesystem>
#include <random>
#include <sstream>
#include <windows.h>
#include <ctime>
#include <processthreadsapi.h>

#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;

void SetColor(WORD attributes) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, attributes);
}

void ResetColor() {
    SetColor(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

std::string generate_aircraft_id() {
    DWORD pid = GetCurrentProcessId();
    return "Plane_" + std::to_string(pid);
}

std::vector<std::string> find_telemetry_files() {
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(fs::current_path())) {
        if (entry.path().extension() == ".txt") {
            files.push_back(entry.path().filename().string());
        }
    }
    return files;
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" ");
    if (first == std::string::npos) return str;
    return str.substr(first);
}

bool shutdown_requested(const fs::path& shutdownFile) {
    return fs::exists(shutdownFile);
}

void send_telemetry(const std::string& server_ip, const std::string& file_path, const std::string& aircraft_id) {
    WSADATA wsa;
    SOCKET client_socket;
    struct sockaddr_in server_addr;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == INVALID_SOCKET) {
        WSACleanup(); return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5000);
    InetPtonA(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

    if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        closesocket(client_socket); WSACleanup(); return;
    }

    SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "[CLIENT] Connected as " << aircraft_id << "\n";
    ResetColor();

    std::ifstream file(file_path);
    if (!file) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] File not found: " << file_path << "\n";
        ResetColor();
        closesocket(client_socket); WSACleanup(); return;
    }

    fs::path shutdownFile = fs::current_path() / "shutdown.txt";
    std::string line; bool firstLine = true;

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (firstLine) { firstLine = false; continue; }

        std::stringstream ss(line);
        std::string timestamp, fuel_remaining;
        if (std::getline(ss, timestamp, ',') && std::getline(ss, fuel_remaining, ',')) {
            std::string data = aircraft_id + "," + timestamp + "," + fuel_remaining + "\n";
            int result = send(client_socket, data.c_str(), static_cast<int>(data.length()), 0);
            if (result == SOCKET_ERROR) break;

            SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::cout << "[SENT] " << data;
            ResetColor();

            for (int i = 0; i < 10; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (shutdown_requested(shutdownFile)) goto exit_loop;
            }
        }
    }
exit_loop:
    shutdown(client_socket, SD_BOTH);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    closesocket(client_socket); WSACleanup();
}

int main() {
    std::string server_ip = "10.144.123.213";
    std::string aircraft_id = generate_aircraft_id();
    auto telemetry_files = find_telemetry_files();

    if (telemetry_files.empty()) {
        SetColor(FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cerr << "[ERROR] No telemetry files found.\n";
        ResetColor(); return 1;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    int choice = std::uniform_int_distribution<>(0, telemetry_files.size() - 1)(gen);

    SetColor(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::cout << "[CLIENT] Selected file: " << telemetry_files[choice] << "\n";
    ResetColor();

    send_telemetry(server_ip, telemetry_files[choice], aircraft_id);
    return 0;
}