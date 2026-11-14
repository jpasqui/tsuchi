// The memory service: blocks a mod reserves, and reads and writes at an address (memory_backend.cpp).
#pragma once

namespace tsuchi {

// Connects the memory service. Called once when the framework starts.
void memory_start();

} // namespace tsuchi
