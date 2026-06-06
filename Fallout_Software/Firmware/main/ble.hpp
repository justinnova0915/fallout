// main/ble.hpp
#pragma once

#include <string>
#include <cstdint>

/**
 * @brief Class for managing Bluetooth Low Energy (BLE) states and communication.
 * 
 * Since The ESP32-S3 only supports BLE, we are using the nimBLE library
 * to manage bluetooth.
 */
class BluetoothManager {
public:
    /**
     * @brief Inits the class
     * @param device_name a unique name for the device. default TALOS-01
     */
    explicit BluetoothManager(const std::string& device_name);
    ~BluetoothManager();

    /**
     * @brief Sends commands as string to the computer
     * @param str the command that you want to send as a string
     */
    void writeString(const std::string& str);

    /**
     * @brief Checks whether or not there is string inside the buffer to read
     * @return True if the buffer is not empty, false otherwise
     */
    bool available() const;

    /**
     * @brief Gets the first datastring in the buffer
     * @param terminator the terminating character
     * @return the first chunk of data as a string
     */
    std::string readStringUntil(char terminator);

private:
    std::string dev_name_;
    void initBluetoothStack();
};