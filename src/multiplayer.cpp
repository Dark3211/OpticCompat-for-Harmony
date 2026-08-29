#include "multiplayer.hpp"
#include "memory.hpp"
#include "runtime.hpp"
#include "log.hpp"

extern "C" void *g_opticcompat_event_original = nullptr;
extern "C" void *g_opticcompat_sound_trampoline = nullptr;

extern "C" void __cdecl opticcompat_dispatch_event(std::uint32_t event_value,
                                                    std::uint32_t killer_id,
                                                    std::uint32_t victim_id,
                                                    std::uint32_t local_id) {
    OpticCompat::Runtime::instance().dispatch_multiplayer_event(event_value, local_id, killer_id, victim_id);
}

extern "C" int __cdecl opticcompat_dispatch_sound(std::uint32_t sound_value) {
    return OpticCompat::Runtime::instance().dispatch_multiplayer_sound(sound_value) ? 1 : 0;
}

#if defined(_M_IX86)
extern "C" __declspec(naked) void opticcompat_event_bridge() {
    __asm {
        pushfd
        pushad
        push edi
        push ecx
        push eax
        push edx
        call opticcompat_dispatch_event
        add esp, 16
        popad
        popfd
        call dword ptr [g_opticcompat_event_original]
        ret
    }
}

extern "C" __declspec(naked) void opticcompat_sound_bridge() {
    __asm {
        pushfd
        pushad
        push esi
        call opticcompat_dispatch_sound
        add esp, 4
        test eax, eax
        jz cancel_sound
        popad
        popfd
        jmp dword ptr [g_opticcompat_sound_trampoline]
    cancel_sound:
        popad
        popfd
        ret
    }
}
#endif

namespace OpticCompat {
    bool MultiplayerHooks::install_event_hook() {
        if(event_installed_) return true;
#if !defined(_M_IX86)
        log_line("Multiplayer event hook requires Win32/x86.");
        return false;
#else
        const Memory::Pattern pattern = {0x52, 0x50, 0xE8, -1, -1, -1, -1, 0x83, 0xC4, 0x10, 0x5F};
        auto *sig = Memory::scan_unique(pattern, "optic multiplayer event");
        if(!sig) return false;
        auto *call = sig + 2;
        if(std::to_integer<unsigned char>(call[0]) != 0xE8) return false;
        std::int32_t rel = 0;
        std::memcpy(&rel, call + 1, sizeof(rel));
        g_opticcompat_event_original = call + 5 + rel;
        if(!Memory::is_readable(g_opticcompat_event_original, 1)) {
            log_line("Original multiplayer event target is invalid.");
            g_opticcompat_event_original = nullptr;
            return false;
        }
        if(!Memory::write_relative_branch(call, 0xE8, reinterpret_cast<void *>(&opticcompat_event_bridge))) {
            g_opticcompat_event_original = nullptr;
            return false;
        }
        event_installed_ = true;
        log_line("Multiplayer event hook installed.");
        return true;
#endif
    }

    bool MultiplayerHooks::install_sound_hook() {
        if(sound_installed_) return true;
#if !defined(_M_IX86)
        log_line("Multiplayer sound hook requires Win32/x86.");
        return false;
#else
        const Memory::Pattern pattern = {0xC6, 0x44, 0x24, 0x04, 0x00, 0x8A, 0x86};
        auto *sig = Memory::scan_unique(pattern, "optic multiplayer sound");
        if(!sig) return false;
        g_opticcompat_sound_trampoline = Memory::make_trampoline(sig, 5, sig + 5);
        if(!g_opticcompat_sound_trampoline) {
            log_line("Could not create multiplayer sound trampoline.");
            return false;
        }
        if(!Memory::write_relative_branch(sig, 0xE9, reinterpret_cast<void *>(&opticcompat_sound_bridge))) {
            VirtualFree(g_opticcompat_sound_trampoline, 0, MEM_RELEASE);
            g_opticcompat_sound_trampoline = nullptr;
            return false;
        }
        sound_installed_ = true;
        log_line("Multiplayer sound hook installed.");
        return true;
#endif
    }

    const char *MultiplayerHooks::event_name(std::uint32_t value) noexcept {
        switch(value) {
            case 1: return "falling dead";
            case 2: return "guardian kill";
            case 3: return "vehicle kill";
            case 4: return "player kill";
            case 5: return "betrayed";
            case 6: return "suicide";
            case 7: return "local double kill";
            case 8: return "local killed player";
            case 9: return "local triple kill";
            case 10: return "local killtacular";
            case 11: return "local killing spree";
            case 12: return "local running riot";
            case 30: return "game time left";
            case 33: return "local ctf score";
            case 34: return "ctf enemy score";
            case 35: return "ctf ally score";
            case 38: return "ctf enemy stole flag";
            case 40: return "ctf enemy returned flag";
            case 41: return "ctf ally stole flag";
            case 42: return "ctf ally returned flag";
            case 43: return "ctf friendly flag idle returned";
            case 44: return "ctf enemy flag idle returned";
            default: return nullptr;
        }
    }

    const char *MultiplayerHooks::sound_name(std::uint32_t value) noexcept {
        static constexpr const char *names[] = {
            "playball", "game over", "one minute to win", "thirty seconds to win",
            "red team one minute to win", "red team thirty seconds to win",
            "blue team one minute to win", "red team thirty seconds to win",
            "red team flag taken", "red team flag returned", "red team flag captured",
            "blue team flag taken", "blue team flag returned", "blue team flag captured",
            "double kill", "triple kill", "killtacular", "running riot", "killing spree",
            "oddball", "race", "slayer", "capture the flag", "warthog", "ghost", "scorpion",
            "countdown timer", "teleporter activate", "flag failure", "countdown for respawn",
            "hill move", "player respawn", "king of the hill", "odd ball", "team race",
            "team slayer", "team king of the hill", "blue team capture the flag",
            "red team capture the flag", "contest", "control", "hill occupied",
            "countdown timer end", "ting", "custom 1", "custom 2", "custom 3", "custom 4",
            "custom 5", "custom 6", "custom 7", "custom 8", "custom 9", "custom 10",
            "custom 11", "custom 12", "custom 13", "custom 14", "custom 15", "custom 16"
        };
        return value < std::size(names) ? names[value] : nullptr;
    }
}
