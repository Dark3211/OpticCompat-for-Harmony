#pragma once
#include "common.hpp"

namespace OpticCompat::Memory {
    using Pattern = std::vector<int>;

    std::vector<std::byte *> scan_executable_sections(const Pattern &pattern);
    std::byte *scan_unique(const Pattern &pattern, const char *name);
    bool write_bytes(void *destination, const void *source, std::size_t size);
    bool write_relative_branch(std::byte *at, std::uint8_t opcode, const void *target);
    void *make_trampoline(const std::byte *original, std::size_t stolen_size, const std::byte *continue_at);
    bool is_readable(const void *address, std::size_t size = sizeof(void *));
}
