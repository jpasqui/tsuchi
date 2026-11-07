// The reflect service, on top of the reader in base/reflect.h.
#pragma once
#include <stdint.h>

#include "tsuchi/core.h"

namespace tsuchi {

// Gives the service the code that does its work. Called once when the framework starts.
void reflect_start();

// Values by path in memory that a type describes, as the service gives and
// takes them. Other services use these for memory that is no object's: a row
// of a table.
TSUCHI_Result read_at(uint64_t type, uint64_t base, const char* path, TSUCHI_Value* out);
TSUCHI_Result write_at(uint64_t type, uint64_t base, const char* path, const TSUCHI_Value* value);
TSUCHI_Result text_at(uint64_t type, uint64_t base, const char* path, char* buffer, uint64_t capacity, uint64_t* out);
TSUCHI_Result write_text_at(uint64_t type, uint64_t base, const char* path, const char* text);
TSUCHI_Result resize_at(uint64_t type, uint64_t base, const char* path, uint32_t count);
// The name of the field at a position in the struct a path leads to. An empty
// path is the type itself.
TSUCHI_Result field_name_at(uint64_t type, uint64_t base, const char* path, uint32_t index, bool parents, char* buffer, uint64_t capacity,
                         uint64_t* out);

// Optional engine-console adapter. The core's host stand-ins need none.
struct ConsoleVariables {
    TSUCHI_Result (*read)(const char*, float*);
    TSUCHI_Result (*hold)(TSUCHI_Mod*, const char*, float);
    TSUCHI_Result (*release)(TSUCHI_Mod*, const char*);
};
void set_console_variables(const ConsoleVariables* backend);
void console_variables_start();
} // namespace tsuchi
