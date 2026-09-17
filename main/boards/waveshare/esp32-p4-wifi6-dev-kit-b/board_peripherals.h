#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct BoardPeripheralStatus {
    std::string ethernet = "正在初始化";
    std::string ethernet_ip = "--";
    std::string gateway = "--";
    std::string wifi = "正在初始化";
    std::string wifi_ip = "--";
    std::string wifi_ssid;
    std::string c6_version = "尚未读取";
    std::string storage = "未插入 SD 卡";
    std::string usb = "正在初始化";
    unsigned audio_interfaces = 0;
    bool wifi_busy = false;
    unsigned scan_revision = 0;
    std::vector<std::string> networks;
};

BoardPeripheralStatus GetBoardPeripheralStatus();
bool RequestBoardWifiScan();
bool RequestBoardWifiConnect(const std::string& ssid, const std::string& password);
bool RequestBoardStorageCheck();

// Callback is delivered outside the status lock on aggregate IP availability changes.
void StartBoardPeripherals(std::function<void(bool)> network_callback);
bool IsBoardNetworkConnected();
bool ReadBoardUsbAudio(int16_t* samples, int count);
void StopBoardUsbAudio();
bool RequestBoardC6Update(const std::string& url);
