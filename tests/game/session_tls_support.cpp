// What the framework's Mbed TLS configuration asks of whatever it is built into, for host tests of sealed
// sessions: memory from this machine's C library and its clock. Sealing uses no randomness from here.
#include <cstdint>
#include <cstdlib>
#include <ctime>

extern "C" void* tsuchi_tls_calloc(size_t count, size_t size) { return std::calloc(count, size); }
extern "C" void tsuchi_tls_free(void* block) { std::free(block); }
extern "C" int64_t tsuchi_tls_time(int64_t* out) {
    const int64_t now = int64_t(std::time(nullptr));
    if (out) *out = now;
    return now;
}
