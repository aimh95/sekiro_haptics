#pragma once

// Which prosthetic tool the player currently has SELECTED, and what is in the
// equipped slots.
//
// This is SELECTION STATE, not use. Switching tools changes it; nothing here
// says a tool was fired, swung, or hit anything. Treating a selection change
// as a use event would be wrong and this class deliberately gives a caller no
// way to do it.
//
// WHAT WAS OBSERVED, AND HOW (2026-09-20, build sha256 637aca52...b856)
// ---------------------------------------------------------------------
//   PlayerGameData + 0x5E8   an array of 8-byte records. The SECOND u32 of
//                            each record is a full EquipParamWeapon row id.
//                            Read live while the user changed slot 2 three
//                            times, and it matched their own account of what
//                            they equipped: 70500 Lazulite Shuriken, then
//                            78000 Loaded Spear. Slots 0 and 1 held 70000
//                            Loaded Shuriken and 75000 Sabimaru throughout.
//   PlayerGameData + 0x518   an EquipGameData (checked by vftable before use)
//   EquipGameData  + 0xA4    u8, the SELECTED slot index. Observed taking 0,
//                            1 and 2 as the user cycled a 3-slot loadout.
//
// The first u32 of each record is NOT interpreted. It held 0x01FF0FFF, 7700
// and 110000 for the three slots -- no reading of it has been verified, so it
// is exposed raw and left unnamed.
//
// WHY PlayerGameData IS SEARCHED FOR RATHER THAN REACHED BY A FIXED PATH
// ----------------------------------------------------------------------
// A breadth-first "nearest EquipMagicData from PlayerIns" returned a DIFFERENT
// instance on consecutive runs, once an object whose fields were all 0xFF.
// Several instances of these classes are live and traversal order is not a
// rule about which is the player's. PlayerGameData, by contrast, had exactly
// one instance reachable from PlayerIns, so it is located by a bounded,
// vftable-validated search -- the same shape of proof SekiroPlayerGuardReader
// uses for the module container. Its children are then taken at fixed offsets
// and EACH is vftable-checked, so a wrong offset fails loudly instead of
// returning a plausible-looking wrong number.
//
// OS-independent: IProcessReader / IProcessInspector only.

#include "sekiro_haptics/process/ExecutableIdentity.hpp"
#include "sekiro_haptics/process/IProcessInspector.hpp"
#include "sekiro_haptics/process/IProcessReader.hpp"
#include "sekiro_haptics/process/SekiroKnownRootResolver.hpp"
#include "sekiro_haptics/process/SekiroPlayerGuardReader.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sekiro_haptics::process {

/// Every address-shaped constant, as data. Defaults are the values observed on
/// this project's build.
struct ProstheticLayout {
    /// WorldChrMan / PlayerIns anchors and their vftables are the same ones the
    /// guard reader already proved, so they are reused rather than retyped.
    PlayerGuardLayout shared;
    std::uint32_t playerGameDataVftableRva = 0x29CC9A0;
    std::uint32_t equipGameDataVftableRva = 0x29C4730;
    /// EquipGameData, from PlayerGameData.
    std::size_t equipGameDataOffset = 0x518;
    /// u8 selected slot index, inside EquipGameData.
    std::size_t selectedSlotOffset = 0xA4;
    /// Equipped slot records, from PlayerGameData.
    std::size_t slotArrayOffset = 0x5E8;
    std::size_t slotStride = 8;
    /// Position of the EquipParamWeapon row id inside a record.
    std::size_t slotIdOffset = 4;
    /// How many records to read. Three prosthetic slots were observed; reading
    /// a couple more costs nothing and shows up as `empty`.
    std::size_t slotCount = 4;
    std::size_t searchWidthBytes = 0x2000;
    int searchDepth = 3;
};

enum class ProstheticReadStatus {
    Ok,
    /// The build's identity gate refused -- no scan was attempted.
    UnsupportedBuild,
    /// WorldChrMan null or wrong type (title screen, loading).
    RootUnavailable,
    PlayerInsUnavailable,
    PlayerGameDataNotFound,
    /// PlayerGameData + equipGameDataOffset was not an EquipGameData.
    EquipGameDataInvalid,
    ReadFailed,
};
const char* ToString(ProstheticReadStatus status);

/// Sentinel the game uses for an empty slot.
inline constexpr std::uint32_t kEmptyEquipId = 0xFFFFFFFFu;

struct ProstheticSlot {
    /// Raw first u32 of the record. NOT interpreted -- see the header comment.
    std::uint32_t raw0 = 0;
    std::uint32_t equipParamWeaponId = kEmptyEquipId;
    bool Empty() const { return equipParamWeaponId == kEmptyEquipId; }
};

struct ProstheticSample {
    ProstheticReadStatus status = ProstheticReadStatus::RootUnavailable;
    /// Bumped whenever PlayerGameData is replaced, so a selection from before
    /// a load or a death is never carried across it.
    std::uint64_t generation = 0;
    std::uint8_t selectedSlot = 0;
    /// kEmptyEquipId when the selected slot is empty or out of range.
    std::uint32_t selectedEquipId = kEmptyEquipId;
    std::vector<ProstheticSlot> slots;
    bool Ok() const { return status == ProstheticReadStatus::Ok; }
};

struct ProstheticReaderStats {
    std::uint64_t polls = 0;
    std::uint64_t ok = 0;
    std::uint64_t failed = 0;
    std::uint64_t playerGameDataSearches = 0;
    std::uint64_t playerGameDataChanges = 0;
};

class SekiroProstheticReader {
public:
    SekiroProstheticReader(IProcessReader& reader, IProcessInspector& inspector,
                           KnownRootSpec worldChrManSpec, ExecutableIdentity expectedIdentity,
                           ExecutableIdentity currentIdentity, std::uintptr_t moduleBaseAddress,
                           ProstheticLayout layout = {});

    RootResolveResult Prime();

    /// One complete read. Never returns a value from an object it did not
    /// re-validate on this call: PlayerGameData is re-checked every poll and
    /// the whole cache is dropped the moment it changes.
    ProstheticSample Poll();

    const ProstheticReaderStats& Stats() const { return stats_; }
    void Invalidate();

private:
    std::optional<std::uintptr_t> ReadPointer(std::uintptr_t address);
    bool HasVftable(std::uintptr_t object, std::uint32_t rva);
    std::optional<std::uintptr_t> FindPlayerGameData(std::uintptr_t playerIns);

    IProcessReader& reader_;
    SekiroKnownRootResolver rootResolver_;
    std::uintptr_t moduleBase_;
    ProstheticLayout layout_;

    bool primed_ = false;
    std::uintptr_t cachedPlayerIns_ = 0;
    std::uintptr_t cachedGameData_ = 0;
    std::uint64_t generation_ = 0;
    ProstheticReaderStats stats_;

    std::vector<std::uint8_t> scratch_;
    std::vector<std::uintptr_t> frontier_;
    std::vector<std::uintptr_t> next_;
    std::vector<std::uintptr_t> seen_;
};

} // namespace sekiro_haptics::process
