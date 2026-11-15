#pragma once

#include <cstddef>
#include <cstdint>

// One program built into the firmware. tools/make_roms.py generates the table.
struct RomEntry {
    const char* name;      // shown in the menu (upper case)
    const uint8_t* data;
    size_t size;
};
