#pragma once

#include <string>

// Board-only network diagnostics for the P4 -> C6 (esp_hosted) uplink.
//   p4 net ping <ipv4|host> [count]        device-side RTT, also under media load
//   p4 net tcp                             one sample of every active TCP PCB
//   p4 net watch <on|off>                  1 s TCP PCB samples for remote port 443
//   p4 net tput <ipv4> <port> <seconds>    raw TCP upload to a LAN sink (link capacity)
bool HandleBoardNetDiagCommand(const std::string& command);
