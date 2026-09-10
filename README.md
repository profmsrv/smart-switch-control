🏠 SmartHomePro
A smart home automation system built using ESP8266 and RS485 for reliable and scalable home control.

✨ Features
Wi-Fi based device control
ESP8266 based controllers
RS485 communication
Mobile and Web control
Physical switch control
Energy monitoring
Optional Firebase cloud integration
Secure user authentication
Scalable relay control
Local hotspot mode
⚙️ Technology
Microcontroller: ESP8266
Communication: RS485
Cloud: Firebase
Connectivity: Wi-Fi
Control: Mobile, Web & Physical Switches
🏗️ How It Works
The main ESP8266 controller communicates with multiple sub-controllers using RS485. Each sub-controller controls connected electrical loads through relays.



Mobile / Web
│
Wi-Fi
│
Main Controller
ESP8266
│
RS485
│
┌───┼────┬────┐
│ │ │ │
Sub Sub Sub Sub
│ │ │ │
Relay Relay Relay Relay
│ │ │ │
Loads Loads Loads Loads
⚡ Energy Monitoring
Monitor current and power consumption of connected electrical loads to understand usage and improve energy efficiency.

☁️ Firebase
Optional Firebase integration allows remote control, authentication, and real-time system monitoring from anywhere.

🚀 Getting Started
Clone this repository.
Open the ESP8266 project in Arduino IDE.
Install the required libraries.
Configure Wi-Fi and Firebase settings.
Upload the firmware to the ESP8266.
Connect the controllers using RS485.
Connect and configure the relay outputs.
⚠️ Safety
This project may involve high-voltage electrical systems. Electrical wiring and installation should only be performed by a qualified professional.

📌 Project Status
🚧 Under Development

❤️ About
SmartHomePro is designed to provide reliable, scalable, and energy-efficient smart home automation using affordable embedded hardware.

Made with ❤️ in India 🇮🇳
