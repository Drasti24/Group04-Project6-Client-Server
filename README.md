# ✈️ Airline Telemetry System

## Overview
This system is designed for the transmission and processing of telemetry data between aircraft (clients) and a server. The client reads telemetry data from a text file, sends it to the server, and the server calculates the average fuel consumption for each aircraft based on the transmitted data.

---

## 🔑 Key Components
- **Client**: Reads telemetry data from a `.txt` file, formats it, and sends it to the server.
- **Server**: Receives the data, processes it, and calculates the average fuel consumption for each aircraft.

---

## ⚙️ Setup Instructions

### 🔧 Prerequisites
Ensure you have the following installed:
- C++ compiler (e.g., Microsoft Visual Studio)
- Wireshark for monitoring network traffic (optional for debugging)

### Steps to Run

1. **Clone the repository**:
   ```bash
   git clone https://github.com/Drasti24/Group04-Project6-Client-Server.git
   ```

2. **Build the Client and Server**:
   - Open the project in your preferred IDE (e.g., Visual Studio).
   - Build the project to generate the executable files for both the client and server.
  
3. **Configure the Server**:
   - The server listens on port 5000. Ensure this port is open on your firewall and network configurations.
   - You may need to adjust network settings to ensure the server can be reached from client machines (e.g., using local IPs in case of Wi-Fi/NAT issues).

4. **Run the server**:
   - In your terminal or IDE, run the server:
    ```
    ./Server
    ```
   - The server should start and listen for incoming connections on port 5000.
  
5. **Run the Client**:
   - On each client machine, run the client executable:
     ```
     ./Client
     ```
   - The client will read only from the telemetry file, send data to the server, and wait for the server to calculate the fuel consumption.

  ---
  
### ℹ️ Additional Notes
- The client sends telemetry data to the server at regular intervals (one second between each packet).
- The server stores fuel data per aircraft and calculates the fuel consumption after receiving the full dataset for each aircraft.
- Fuel consumption is calculated as the difference between the first and last fuel reading divided by the number of time units.

---

### 👩‍💻👨‍💻 Contributors
- [Rudra Patel](https://github.com/RudraPatelhere) - 👨‍💻Developer & 🔧Tester 
- [Jiya Pandit](https://github.com/jiyapandit) - 👩‍💻Developer & 🔧Tester
- [Komalpreet Kaur](https://github.com/komalpreet0) - 👩‍💻Developer & 🔧Tester
- [Drasti Patel](https://github.com/Drasti24) - 👩‍💻Developer & 🔧Tester


---

## 📝 License

This project was developed as part of an academic assignment and is intended for educational use only.
