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

#pragma comment(lib, "ws2_32.lib")

namespace fs = std::filesystem;

// Generate a unique aircraft ID (randomized for now)
std::string generate_aircraft_id()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(1000, 9999);
    return "Plane_" + std::to_string(dist(gen));
}

// Find available telemetry files
std::vector<std::string> find_telemetry_files()
{
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator("."))
    {
        if (entry.path().extension() == ".txt")
        {
            files.push_back(entry.path().filename().string());
        }
    }
    return files;
}

// Trim leading spaces from a string
std::string trim(const std::string& str)
{
    size_t first = str.find_first_not_of(" ");
    if (first == std::string::npos) return str;
    return str.substr(first);
}

// Function to send telemetry data with a message delimiter
void send_telemetry(const std::string& server_ip, const std::string& file_path, const std::string& aircraft_id) {
    WSADATA wsa;
    SOCKET client_socket;
    struct sockaddr_in server_addr;

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[ERROR] WSAStartup failed.\n";
        return;
    }

    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == INVALID_SOCKET) {
        std::cerr << "[ERROR] Failed to create socket.\n";
        WSACleanup();
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5000);

    if (InetPtonA(AF_INET, server_ip.c_str(), &server_addr.sin_addr) != 1) {
        std::cerr << "[ERROR] Invalid IP address format.\n";
        closesocket(client_socket);
        WSACleanup();
        return;
    }

    if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "[ERROR] Could not connect to server.\n";
        closesocket(client_socket);
        WSACleanup();
        return;
    }

    std::cout << "[CLIENT] Connected to server as " << aircraft_id << ".\n";

    std::ifstream file(file_path);
    if (!file) {
        std::cerr << "[ERROR] File not found: " << file_path << "\n";
        closesocket(client_socket);
        WSACleanup();
        return;
    }

    std::string line;
    bool firstLine = true;

    while (std::getline(file, line)) {
        line = trim(line);

        if (line.empty()) continue;

        // Skip the header line if present
        if (firstLine) {
            firstLine = false;
            continue;
        }

        std::stringstream ss(line);
        std::string timestamp, fuel_remaining;

        if (std::getline(ss, timestamp, ',') && std::getline(ss, fuel_remaining, ',')) {
            // Append a newline as a delimiter at the end of the message.
            std::string data_to_send = aircraft_id + "," + timestamp + "," + fuel_remaining + "\n";
            int result = send(client_socket, data_to_send.c_str(), data_to_send.length(), 0);
            if (result == SOCKET_ERROR) {
                std::cerr << "[ERROR] Failed to send data. Server might be down.\n";
                break; // Stop sending further if server is unreachable
            }
            std::cout << "[SENT] " << data_to_send;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    // Gracefully close connection
    shutdown(client_socket, SD_BOTH);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    closesocket(client_socket);
    WSACleanup();
}


int main()
{
    // HARDCODED IP 
    std::string server_ip = "192.168.25.108";

    std::string aircraft_id = generate_aircraft_id();
    std::vector<std::string> telemetry_files = find_telemetry_files();

    if (telemetry_files.empty())
    {
        std::cerr << "[ERROR] No telemetry files found.\n";
        return 1;
    }

    // Let the user choose a telemetry file randomly
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, telemetry_files.size() - 1);
    int choice = dist(gen);

    std::cout << "[CLIENT] Selected file: " << telemetry_files[choice] << "\n";

    send_telemetry(server_ip, telemetry_files[choice], aircraft_id);
    return 0;
}
