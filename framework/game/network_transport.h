#pragma once
#include <stdint.h>
#include "tsuchi/net.h"
// Framework-internal side of net. A target sets one backend at startup;
// net_backend.cpp validates every public call and owns handles and ownership, so
// a backend only moves bytes. Descriptors are the backend's own (>= 0).
namespace tsuchi {
struct NetBackend {
    bool (*available)();
    bool (*connected)();
    int32_t (*udp_open)(uint16_t port, bool broadcast);        // descriptor, -3 port in use, or -1
    int32_t (*send_to)(int32_t fd, const uint8_t ip[4], uint16_t port,
                       const void* data, uint32_t size);       // 0, or -1
    // Bytes copied (a longer datagram is truncated), -2 when none waits, -1 on error.
    int32_t (*receive_from)(int32_t fd, uint8_t ip[4], uint16_t* port,
                            void* buffer, uint32_t capacity);
    int32_t (*local)(int32_t fd, uint8_t ip[4], uint16_t* port); // 0, or -1
    void (*close)(int32_t fd);
    int32_t (*resolve)(const char* host, uint8_t ip[4]);        // 0, 1 unknown, -1 error
};
TSUCHI_Result network_numeric_address(const char*,uint32_t,TSUCHI_NetAddress*);
void network_native_start();
void set_network_transport(const NetBackend*);
const NetBackend* network_transport();
}
