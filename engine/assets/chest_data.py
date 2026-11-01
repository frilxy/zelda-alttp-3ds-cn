# chest_data.py — ALTTPR chest-name catalog (MIT) + a reader for the generated
# chest table. CHEST_NAME_BY_ROM_ADDR maps each chest's address to its ALTTPR
# name/type; the table itself is a generated artifact (chest_table.gen.bin,
# written by extract_resources.dump_chest_table). get_chest_lookup_rows() joins
# the two. Absent artifact (e.g. CI) -> empty lookup.

from __future__ import annotations

from pathlib import Path


# 168 entries x 3 bytes: uint16_le room_word (bit 15 = big-chest) + uint8 item.
# The runtime reads the room-sorted copy from zelda3_assets.dat
# (kDungeonRoomChests); per-room order matches, so the chest_ordinal
# (tile - 0x58) computed here matches the runtime.
CHEST_TABLE_BASE_ADDR = 0xE96E  # low-bank addr of entry 0's room-word
CHEST_TABLE_ENTRY_COUNT = 168
CHEST_TABLE_GEN_PATH = Path(__file__).resolve().parent / "rando" / "chest_table.gen.bin"


def load_vanilla_chest_table() -> bytes | None:
    """Read the generated chest table (504 bytes), or None if the artifact is absent."""
    path = CHEST_TABLE_GEN_PATH
    if not path.is_file():
        return None
    data = path.read_bytes()
    if len(data) % 3 != 0 or len(data) == 0:
        raise RuntimeError(
            "%s is %d bytes, not a whole number of 3-byte chest entries — "
            "re-run asset extraction." % (path, len(data))
        )
    return data


# ALTTPR (MIT) chest declarations from app/Region/Standard/**.php: rom_addr ->
# (name, type), PHP file:line cited per entry. Validated by codegen against
# location_registry.yaml. `Chest Game` (0xEDA8) is out-of-table (minigame;
# OpenMiniGameChest) and is not emitted into the lookup.
CHEST_NAME_BY_ROM_ADDR = {
    0xE96E: ("Sewers - Dark Cross", "Chest"),  # app/Region/Standard/HyruleCastleEscape.php:45
    0xE971: ("Secret Passage", "Chest"),  # app/Region/Standard/HyruleCastleEscape.php:50
    0xE974: ("Hyrule Castle - Boomerang Chest", "Chest"),  # app/Region/Standard/HyruleCastleEscape.php:46
    0xE977: ("Eastern Palace - Compass Chest", "Chest"),  # app/Region/Standard/EasternPalace.php:51
    0xE97A: ("King's Tomb", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:33
    0xE97D: ("Eastern Palace - Big Chest", "BigChest"),  # app/Region/Standard/EasternPalace.php:52
    0xE980: ("Pyramid Fairy - Left", "Chest"),  # app/Region/Standard/DarkWorld/NorthEast.php:40
    0xE983: ("Pyramid Fairy - Right", "Chest"),  # app/Region/Standard/DarkWorld/NorthEast.php:41
    0xE986: ("Swamp Palace - Map Chest", "Chest"),  # app/Region/Standard/SwampPalace.php:53
    0xE989: ("Swamp Palace - Big Chest", "BigChest"),  # app/Region/Standard/SwampPalace.php:51
    0xE98C: ("Floodgate Chest", "Chest"),  # app/Region/Standard/LightWorld/South.php:32
    0xE98F: ("Desert Palace - Big Chest", "BigChest"),  # app/Region/Standard/DesertPalace.php:55
    0xE992: ("Skull Woods - Compass Chest", "Chest"),  # app/Region/Standard/SkullWoods.php:59
    0xE995: ("Ice Palace - Freezor Chest", "Chest"),  # app/Region/Standard/IcePalace.php:54
    0xE998: ("Skull Woods - Big Chest", "BigChest"),  # app/Region/Standard/SkullWoods.php:57
    0xE99B: ("Skull Woods - Map Chest", "Chest"),  # app/Region/Standard/SkullWoods.php:60
    0xE99E: ("Skull Woods - Big Key Chest", "Chest"),  # app/Region/Standard/SkullWoods.php:58
    0xE9A1: ("Skull Woods - Pot Prison", "Chest"),  # app/Region/Standard/SkullWoods.php:62
    0xE9A4: ("Ice Palace - Big Key Chest", "Chest"),  # app/Region/Standard/IcePalace.php:50
    0xE9AA: ("Ice Palace - Big Chest", "BigChest"),  # app/Region/Standard/IcePalace.php:56
    0xE9AD: ("Tower of Hera - Map Chest", "Chest"),  # app/Region/Standard/TowerOfHera.php:55
    0xE9B0: ("Waterfall Fairy - Left", "Chest"),  # app/Region/Standard/LightWorld/NorthEast.php:39
    0xE9B3: ("Eastern Palace - Cannonball Chest", "Chest"),  # app/Region/Standard/EasternPalace.php:53
    0xE9B6: ("Desert Palace - Map Chest", "Chest"),  # app/Region/Standard/DesertPalace.php:56
    0xE9B9: ("Eastern Palace - Big Key Chest", "Chest"),  # app/Region/Standard/EasternPalace.php:54
    0xE9BC: ("Link's House", "Chest"),  # app/Region/Standard/LightWorld/South.php:33
    0xE9BF: ("Spiral Cave", "Chest"),  # app/Region/Standard/LightWorld/DeathMountain/East.php:32
    0xE9C2: ("Desert Palace - Big Key Chest", "Chest"),  # app/Region/Standard/DesertPalace.php:58
    0xE9C5: ("Mimic Cave", "Chest"),  # app/Region/Standard/LightWorld/DeathMountain/East.php:33
    0xE9C8: ("Skull Woods - Pinball Room", "Chest"),  # app/Region/Standard/SkullWoods.php:63
    0xE9CB: ("Desert Palace - Compass Chest", "Chest"),  # app/Region/Standard/DesertPalace.php:59
    0xE9CE: ("Kakariko Tavern", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:34
    0xE9D1: ("Waterfall Fairy - Right", "Chest"),  # app/Region/Standard/LightWorld/NorthEast.php:40
    0xE9D4: ("Ice Palace - Compass Chest", "Chest"),  # app/Region/Standard/IcePalace.php:51
    0xE9DA: ("Misery Mire - Spike Chest", "Chest"),  # app/Region/Standard/MiseryMire.php:56
    0xE9DD: ("Ice Palace - Map Chest", "Chest"),  # app/Region/Standard/IcePalace.php:52
    0xE9E0: ("Ice Palace - Spike Room", "Chest"),  # app/Region/Standard/IcePalace.php:53
    0xE9E3: ("Ice Palace - Iced T Room", "Chest"),  # app/Region/Standard/IcePalace.php:55
    0xE9E6: ("Tower of Hera - Big Key Chest", "Chest"),  # app/Region/Standard/TowerOfHera.php:53
    0xE9E9: ("Chicken House", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:35
    0xE9EC: ("Brewery", "Chest"),  # app/Region/Standard/DarkWorld/NorthWest.php:32
    0xE9EF: ("C-Shaped House", "Chest"),  # app/Region/Standard/DarkWorld/NorthWest.php:33
    0xE9F2: ("Aginah's Cave", "Chest"),  # app/Region/Standard/LightWorld/South.php:34
    0xE9F5: ("Eastern Palace - Map Chest", "Chest"),  # app/Region/Standard/EasternPalace.php:55
    0xE9F8: ("Tower of Hera - Big Chest", "BigChest"),  # app/Region/Standard/TowerOfHera.php:57
    0xE9FB: ("Tower of Hera - Compass Chest", "Chest"),  # app/Region/Standard/TowerOfHera.php:56
    0xE9FE: ("Skull Woods - Bridge Room", "Chest"),  # app/Region/Standard/SkullWoods.php:61
    0xEA01: ("Thieves' Town - Map Chest", "Chest"),  # app/Region/Standard/ThievesTown.php:52
    0xEA04: ("Thieves' Town - Big Key Chest", "Chest"),  # app/Region/Standard/ThievesTown.php:51
    0xEA07: ("Thieves' Town - Compass Chest", "Chest"),  # app/Region/Standard/ThievesTown.php:53
    0xEA0A: ("Thieves' Town - Ambush Chest", "Chest"),  # app/Region/Standard/ThievesTown.php:54
    0xEA0D: ("Thieves' Town - Attic", "Chest"),  # app/Region/Standard/ThievesTown.php:50
    0xEA10: ("Thieves' Town - Big Chest", "BigChest"),  # app/Region/Standard/ThievesTown.php:55
    0xEA13: ("Thieves' Town - Blind's Cell", "Chest"),  # app/Region/Standard/ThievesTown.php:56
    0xEA16: ("Turtle Rock - Chain Chomps", "Chest"),  # app/Region/Standard/TurtleRock.php:60
    0xEA19: ("Turtle Rock - Big Chest", "BigChest"),  # app/Region/Standard/TurtleRock.php:64
    0xEA1C: ("Turtle Rock - Roller Room - Left", "Chest"),  # app/Region/Standard/TurtleRock.php:62
    0xEA1F: ("Turtle Rock - Roller Room - Right", "Chest"),  # app/Region/Standard/TurtleRock.php:63
    0xEA22: ("Turtle Rock - Compass Chest", "Chest"),  # app/Region/Standard/TurtleRock.php:61
    0xEA25: ("Turtle Rock - Big Key Chest", "Chest"),  # app/Region/Standard/TurtleRock.php:65
    0xEA28: ("Turtle Rock - Eye Bridge - Top Right", "Chest"),  # app/Region/Standard/TurtleRock.php:70
    0xEA2B: ("Turtle Rock - Eye Bridge - Top Left", "Chest"),  # app/Region/Standard/TurtleRock.php:69
    0xEA2E: ("Turtle Rock - Eye Bridge - Bottom Right", "Chest"),  # app/Region/Standard/TurtleRock.php:68
    0xEA31: ("Turtle Rock - Eye Bridge - Bottom Left", "Chest"),  # app/Region/Standard/TurtleRock.php:67
    0xEA34: ("Turtle Rock - Crystaroller Room", "Chest"),  # app/Region/Standard/TurtleRock.php:66
    0xEA37: ("Palace of Darkness - Big Key Chest", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:51
    0xEA3A: ("Palace of Darkness - The Arena - Ledge", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:52
    0xEA3D: ("Palace of Darkness - The Arena - Bridge", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:53
    0xEA40: ("Palace of Darkness - Big Chest", "BigChest"),  # app/Region/Standard/PalaceOfDarkness.php:56
    0xEA43: ("Palace of Darkness - Compass Chest", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:57
    0xEA46: ("Palace of Darkness - Harmless Hellway", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:58
    0xEA49: ("Palace of Darkness - Stalfos Basement", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:54
    0xEA4C: ("Palace of Darkness - Dark Basement - Left", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:59
    0xEA4F: ("Palace of Darkness - Dark Basement - Right", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:60
    0xEA52: ("Palace of Darkness - Map Chest", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:55
    0xEA55: ("Palace of Darkness - Dark Maze - Top", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:61
    0xEA58: ("Palace of Darkness - Dark Maze - Bottom", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:62
    0xEA5B: ("Palace of Darkness - Shooter Room", "Chest"),  # app/Region/Standard/PalaceOfDarkness.php:50
    0xEA5E: ("Misery Mire - Main Lobby", "Chest"),  # app/Region/Standard/MiseryMire.php:51
    0xEA61: ("Misery Mire - Bridge Chest", "Chest"),  # app/Region/Standard/MiseryMire.php:54
    0xEA64: ("Misery Mire - Compass Chest", "Chest"),  # app/Region/Standard/MiseryMire.php:53
    0xEA67: ("Misery Mire - Big Chest", "BigChest"),  # app/Region/Standard/MiseryMire.php:50
    0xEA6A: ("Misery Mire - Map Chest", "Chest"),  # app/Region/Standard/MiseryMire.php:55
    0xEA6D: ("Misery Mire - Big Key Chest", "Chest"),  # app/Region/Standard/MiseryMire.php:52
    0xEA73: ("Mire Shed - Left", "Chest"),  # app/Region/Standard/DarkWorld/Mire.php:32
    0xEA76: ("Mire Shed - Right", "Chest"),  # app/Region/Standard/DarkWorld/Mire.php:33
    0xEA79: ("Sanctuary", "Chest"),  # app/Region/Standard/HyruleCastleEscape.php:41
    0xEA7C: ("Superbunny Cave - Top", "Chest"),  # app/Region/Standard/DarkWorld/DeathMountain/East.php:32
    0xEA7F: ("Superbunny Cave - Bottom", "Chest"),  # app/Region/Standard/DarkWorld/DeathMountain/East.php:33
    0xEA82: ("Sahasrahla's Hut - Left", "Chest"),  # app/Region/Standard/LightWorld/NorthEast.php:32
    0xEA85: ("Sahasrahla's Hut - Middle", "Chest"),  # app/Region/Standard/LightWorld/NorthEast.php:33
    0xEA88: ("Sahasrahla's Hut - Right", "Chest"),  # app/Region/Standard/LightWorld/NorthEast.php:34
    0xEA8B: ("Spike Cave", "Chest"),  # app/Region/Standard/DarkWorld/DeathMountain/West.php:32
    0xEA8E: ("Kakariko Well - Top", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:36
    0xEA91: ("Kakariko Well - Left", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:37
    0xEA94: ("Kakariko Well - Middle", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:38
    0xEA97: ("Kakariko Well - Right", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:39
    0xEA9A: ("Kakariko Well - Bottom", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:40
    0xEA9D: ("Swamp Palace - Entrance", "Chest"),  # app/Region/Standard/SwampPalace.php:50
    0xEAA0: ("Swamp Palace - Compass Chest", "Chest"),  # app/Region/Standard/SwampPalace.php:55
    0xEAA3: ("Swamp Palace - West Chest", "Chest"),  # app/Region/Standard/SwampPalace.php:54
    0xEAA6: ("Swamp Palace - Big Key Chest", "Chest"),  # app/Region/Standard/SwampPalace.php:52
    0xEAA9: ("Swamp Palace - Flooded Room - Left", "Chest"),  # app/Region/Standard/SwampPalace.php:56
    0xEAAC: ("Swamp Palace - Flooded Room - Right", "Chest"),  # app/Region/Standard/SwampPalace.php:57
    0xEAAF: ("Swamp Palace - Waterfall Room", "Chest"),  # app/Region/Standard/SwampPalace.php:58
    0xEAB2: ("Castle Tower - Dark Maze", "Chest"),  # app/Region/Standard/HyruleCastleTower.php:42
    0xEAB5: ("Castle Tower - Room 03", "Chest"),  # app/Region/Standard/HyruleCastleTower.php:41
    0xEAB8: ("Ganon's Tower - DMs Room - Top Left", "Chest"),  # app/Region/Standard/GanonsTower.php:54
    0xEABB: ("Ganon's Tower - DMs Room - Top Right", "Chest"),  # app/Region/Standard/GanonsTower.php:55
    0xEABE: ("Ganon's Tower - DMs Room - Bottom Left", "Chest"),  # app/Region/Standard/GanonsTower.php:56
    0xEAC1: ("Ganon's Tower - DMs Room - Bottom Right", "Chest"),  # app/Region/Standard/GanonsTower.php:57
    0xEAC4: ("Ganon's Tower - Randomizer Room - Top Left", "Chest"),  # app/Region/Standard/GanonsTower.php:58
    0xEAC7: ("Ganon's Tower - Randomizer Room - Top Right", "Chest"),  # app/Region/Standard/GanonsTower.php:59
    0xEACA: ("Ganon's Tower - Randomizer Room - Bottom Left", "Chest"),  # app/Region/Standard/GanonsTower.php:60
    0xEACD: ("Ganon's Tower - Randomizer Room - Bottom Right", "Chest"),  # app/Region/Standard/GanonsTower.php:61
    0xEAD0: ("Ganon's Tower - Firesnake Room", "Chest"),  # app/Region/Standard/GanonsTower.php:62
    0xEAD3: ("Ganon's Tower - Map Chest", "Chest"),  # app/Region/Standard/GanonsTower.php:63
    0xEAD6: ("Ganon's Tower - Big Chest", "BigChest"),  # app/Region/Standard/GanonsTower.php:64
    0xEAD9: ("Ganon's Tower - Hope Room - Left", "Chest"),  # app/Region/Standard/GanonsTower.php:65
    0xEADC: ("Ganon's Tower - Hope Room - Right", "Chest"),  # app/Region/Standard/GanonsTower.php:66
    0xEADF: ("Ganon's Tower - Bob's Chest", "Chest"),  # app/Region/Standard/GanonsTower.php:67
    0xEAE2: ("Ganon's Tower - Tile Room", "Chest"),  # app/Region/Standard/GanonsTower.php:68
    0xEAE5: ("Ganon's Tower - Compass Room - Top Left", "Chest"),  # app/Region/Standard/GanonsTower.php:69
    0xEAE8: ("Ganon's Tower - Compass Room - Top Right", "Chest"),  # app/Region/Standard/GanonsTower.php:70
    0xEAEB: ("Ganon's Tower - Compass Room - Bottom Left", "Chest"),  # app/Region/Standard/GanonsTower.php:71
    0xEAEE: ("Ganon's Tower - Compass Room - Bottom Right", "Chest"),  # app/Region/Standard/GanonsTower.php:72
    0xEAF1: ("Ganon's Tower - Big Key Chest", "Chest"),  # app/Region/Standard/GanonsTower.php:73
    0xEAF4: ("Ganon's Tower - Big Key Room - Left", "Chest"),  # app/Region/Standard/GanonsTower.php:74
    0xEAF7: ("Ganon's Tower - Big Key Room - Right", "Chest"),  # app/Region/Standard/GanonsTower.php:75
    0xEAFD: ("Ganon's Tower - Mini Helmasaur Room - Left", "Chest"),  # app/Region/Standard/GanonsTower.php:76
    0xEB00: ("Ganon's Tower - Mini Helmasaur Room - Right", "Chest"),  # app/Region/Standard/GanonsTower.php:77
    0xEB03: ("Ganon's Tower - Pre-Moldorm Chest", "Chest"),  # app/Region/Standard/GanonsTower.php:78
    0xEB06: ("Ganon's Tower - Moldorm Chest", "Chest"),  # app/Region/Standard/GanonsTower.php:79
    0xEB09: ("Hyrule Castle - Zelda's Cell", "Chest"),  # app/Region/Standard/HyruleCastleEscape.php:48
    0xEB0C: ("Hyrule Castle - Map Chest", "Chest"),  # app/Region/Standard/HyruleCastleEscape.php:47
    0xEB0F: ("Blind's Hideout - Top", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:41
    0xEB12: ("Blind's Hideout - Left", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:42
    0xEB15: ("Blind's Hideout - Right", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:43
    0xEB18: ("Blind's Hideout - Far Left", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:44
    0xEB1B: ("Blind's Hideout - Far Right", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:45
    0xEB1E: ("Hype Cave - Top", "Chest"),  # app/Region/Standard/DarkWorld/South.php:32
    0xEB21: ("Hype Cave - Middle Right", "Chest"),  # app/Region/Standard/DarkWorld/South.php:33
    0xEB24: ("Hype Cave - Middle Left", "Chest"),  # app/Region/Standard/DarkWorld/South.php:34
    0xEB27: ("Hype Cave - Bottom", "Chest"),  # app/Region/Standard/DarkWorld/South.php:35
    0xEB2A: ("Paradox Cave Lower - Far Left", "Chest"),  # app/Region/Standard/LightWorld/DeathMountain/East.php:34
    0xEB2D: ("Paradox Cave Lower - Left", "Chest"),  # app/Region/Standard/LightWorld/DeathMountain/East.php:35
    0xEB30: ("Paradox Cave Lower - Right", "Chest"),  # app/Region/Standard/LightWorld/DeathMountain/East.php:36
    0xEB33: ("Paradox Cave Lower - Far Right", "Chest"),  # app/Region/Standard/LightWorld/DeathMountain/East.php:37
    0xEB36: ("Paradox Cave Lower - Middle", "Chest"),  # app/Region/Standard/LightWorld/DeathMountain/East.php:38
    0xEB39: ("Paradox Cave Upper - Left", "Chest"),  # app/Region/Standard/LightWorld/DeathMountain/East.php:39
    0xEB3C: ("Paradox Cave Upper - Right", "Chest"),  # app/Region/Standard/LightWorld/DeathMountain/East.php:40
    0xEB3F: ("Pegasus Rocks", "Chest"),  # app/Region/Standard/LightWorld/NorthWest.php:46
    0xEB42: ("Mini Moldorm Cave - Far Left", "Chest"),  # app/Region/Standard/LightWorld/South.php:35
    0xEB45: ("Mini Moldorm Cave - Left", "Chest"),  # app/Region/Standard/LightWorld/South.php:36
    0xEB48: ("Mini Moldorm Cave - Right", "Chest"),  # app/Region/Standard/LightWorld/South.php:37
    0xEB4B: ("Mini Moldorm Cave - Far Right", "Chest"),  # app/Region/Standard/LightWorld/South.php:38
    0xEB4E: ("Ice Rod Cave", "Chest"),  # app/Region/Standard/LightWorld/South.php:39
    0xEB51: ("Hookshot Cave - Top Right", "Chest"),  # app/Region/Standard/DarkWorld/DeathMountain/East.php:34
    0xEB54: ("Hookshot Cave - Top Left", "Chest"),  # app/Region/Standard/DarkWorld/DeathMountain/East.php:35
    0xEB57: ("Hookshot Cave - Bottom Left", "Chest"),  # app/Region/Standard/DarkWorld/DeathMountain/East.php:36
    0xEB5A: ("Hookshot Cave - Bottom Right", "Chest"),  # app/Region/Standard/DarkWorld/DeathMountain/East.php:37
    0xEB5D: ("Sewers - Secret Room - Left", "Chest"),  # app/Region/Standard/HyruleCastleEscape.php:42
    0xEB60: ("Sewers - Secret Room - Middle", "Chest"),  # app/Region/Standard/HyruleCastleEscape.php:43
    0xEB63: ("Sewers - Secret Room - Right", "Chest"),  # app/Region/Standard/HyruleCastleEscape.php:44
    # Chest Game (DarkWorld/NorthWest.php:34) at 0xEDA8 — minigame, NOT in the
    # chest table. Intentionally absent from the lookup; runtime path is
    # OpenMiniGameChest (src/dungeon.c:5733). Dispatch is tasks.md S6.8.
}


def get_chest_lookup_rows():
    """(room, ordinal, name, type, item, big, addr) per named chest.

    (room, ordinal) is the runtime dispatch key; ordinal is the N-th entry in
    the table matching that room. Empty list if the artifact is absent (CI).
    """
    table = load_vanilla_chest_table()
    if table is None:
        return []
    entry_count = len(table) // 3
    rows = []
    room_ordinals: dict[int, int] = {}
    for i in range(entry_count):
        addr = CHEST_TABLE_BASE_ADDR + i * 3
        b0 = table[i * 3]
        b1 = table[i * 3 + 1]
        item = table[i * 3 + 2]
        room_word = b0 | (b1 << 8)
        room = room_word & 0x7FFF
        big = (room_word & 0x8000) != 0
        ord_ = room_ordinals.get(room, 0)
        room_ordinals[room] = ord_ + 1
        entry = CHEST_NAME_BY_ROM_ADDR.get(addr)
        if entry is None:
            continue
        name, ctype = entry
        # Sanity: PHP-declared BigChest flag must agree with ROM bit-15.
        php_big = ctype == "BigChest"
        if php_big != big:
            raise RuntimeError(
                "chest big-flag mismatch at addr 0x%04X (%s): PHP big=%s, ROM big=%s"
                % (addr, name, php_big, big)
            )
        rows.append((room, ord_, name, ctype, item, big, addr))
    return rows
