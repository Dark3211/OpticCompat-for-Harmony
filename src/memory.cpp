#include "memory.hpp"
#include "log.hpp"
#include <cstring>

namespace OpticCompat::Memory {
    static bool region_contains(const std::byte *base, std::size_t size, const std::byte *ptr, std::size_t wanted) {
        if(!base || !ptr || wanted > size) return false;
        const auto begin = reinterpret_cast<std::uintptr_t>(base);
        const auto end = begin + size;
        const auto p = reinterpret_cast<std::uintptr_t>(ptr);
        return p >= begin && p <= end && wanted <= end - p;
    }

    std::vector<std::byte *> scan_executable_sections(const Pattern &pattern) {
        std::vector<std::byte *> matches;
        if(pattern.empty()) return matches;

        auto *module = reinterpret_cast<std::byte *>(GetModuleHandleW(nullptr));
        if(!module) return matches;

        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(module);
        if(dos->e_magic != IMAGE_DOS_SIGNATURE) return matches;
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(module + dos->e_lfanew);
        if(nt->Signature != IMAGE_NT_SIGNATURE) return matches;

        const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
        auto *section = IMAGE_FIRST_SECTION(nt);
        for(unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
            if((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
            auto *start = module + section->VirtualAddress;
            std::size_t size = std::max<std::size_t>(section->Misc.VirtualSize, section->SizeOfRawData);
            if(!region_contains(module, image_size, start, 1)) continue;
            const auto max_size = image_size - static_cast<std::size_t>(start - module);
            size = std::min(size, max_size);
            if(size < pattern.size()) continue;

            for(std::size_t offset = 0; offset + pattern.size() <= size; ++offset) {
                bool ok = true;
                for(std::size_t p = 0; p < pattern.size(); ++p) {
                    if(pattern[p] >= 0 && std::to_integer<unsigned char>(start[offset + p]) != static_cast<unsigned char>(pattern[p])) {
                        ok = false;
                        break;
                    }
                }
                if(ok) matches.push_back(start + offset);
            }
        }
        return matches;
    }

    std::byte *scan_unique(const Pattern &pattern, const char *name) {
        auto matches = scan_executable_sections(pattern);
        if(matches.size() != 1) {
            log_line("Signature '%s' expected 1 match, found %zu.", name ? name : "?", matches.size());
            return nullptr;
        }
        return matches.front();
    }

    bool write_bytes(void *destination, const void *source, std::size_t size) {
        if(!destination || !source || size == 0) return false;
        DWORD old_protect = 0;
        if(!VirtualProtect(destination, size, PAGE_EXECUTE_READWRITE, &old_protect)) return false;
        std::memcpy(destination, source, size);
        FlushInstructionCache(GetCurrentProcess(), destination, size);
        DWORD ignored = 0;
        VirtualProtect(destination, size, old_protect, &ignored);
        return true;
    }

    bool write_relative_branch(std::byte *at, std::uint8_t opcode, const void *target) {
        if(!at || !target) return false;
        std::array<std::byte, 5> patch{};
        patch[0] = static_cast<std::byte>(opcode);
        const auto from = reinterpret_cast<std::uintptr_t>(at + 5);
        const auto to = reinterpret_cast<std::uintptr_t>(target);
        const std::int32_t relative = static_cast<std::int32_t>(to - from);
        std::memcpy(patch.data() + 1, &relative, sizeof(relative));
        return write_bytes(at, patch.data(), patch.size());
    }

    void *make_trampoline(const std::byte *original, std::size_t stolen_size, const std::byte *continue_at) {
        if(!original || stolen_size == 0 || !continue_at) return nullptr;
        const std::size_t total = stolen_size + 5;
        auto *mem = reinterpret_cast<std::byte *>(VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if(!mem) return nullptr;
        std::memcpy(mem, original, stolen_size);
        const auto from = reinterpret_cast<std::uintptr_t>(mem + stolen_size + 5);
        const auto to = reinterpret_cast<std::uintptr_t>(continue_at);
        const std::int32_t rel = static_cast<std::int32_t>(to - from);
        mem[stolen_size] = static_cast<std::byte>(0xE9);
        std::memcpy(mem + stolen_size + 1, &rel, sizeof(rel));
        FlushInstructionCache(GetCurrentProcess(), mem, total);
        return mem;
    }

    bool is_readable(const void *address, std::size_t size) {
        if(!address || size == 0) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if(VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if(mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || mbi.Protect == PAGE_NOACCESS) return false;
        const auto begin = reinterpret_cast<std::uintptr_t>(address);
        const auto region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return begin <= region_end && size <= region_end - begin;
    }
}
