#!/usr/bin/env python3
"""rando_logic_gen.py — Phase A logic codegen.

Per `randomizer-logic / Logic YAML schema and worked examples` and tasks.md
§3.5. Reads:

  - assets/rando/op_registry.yaml          (task 3.4 — op-code IDs)
  - assets/rando/item_registry.yaml        (task 3.2 — item IDs)
  - assets/rando/location_registry.yaml    (task 3.1 — location IDs)
  - assets/rando/hint_metadata.yaml        (Hints v2 categories/aliases)
  - assets/rando/hint_metadata.lock.json   (approved algorithm/text fingerprints)
  - assets/rando/hint_registry_contract.json (merged-registry provenance)
  - assets/rando/logic.schema.yaml         (task 3.5a — schema)
  - assets/rando/macros.yaml               (task 3.4a — named macros) [optional]
  - assets/rando/logic.yaml                (task 3.3 — graph)          [optional]

Emits:

  - src/rando/location_ids.h               (#define LOC_<name> <id>)
  - src/rando/item_ids.h                   (#define ITEM_<name> <id>)
  - src/rando/hint_metadata.h              (Hints v2 generated metadata)
  - src/rando/logic_data.c                 (LocationDef[], RegionDef[],
                                             EdgeDef[], predicate streams)

Discipline (per CLAUDE.md claim-grounding and randomizer-core / Byte-order pin):
  - Deterministic output: iteration uses sorted IDs.
  - No `rand`, no wall-clock, no float.
  - All multi-byte integers in emitted byte streams are little-endian.
  - Phase B ops (OP_TRICK / OP_DIFFICULTY_AT_LEAST / OP_GLITCH_LEVEL_AT_LEAST)
    rejected in Phase A logic.yaml by the well-formedness pass (task 3.10).

Bytecode format (recursively-evaluable, no length prefixes — recursion depth
is bounded by the inline-complexity check):

  HAS_ITEM       -> <op:u8> <item_id:u16_le>
  HAS_AMOUNT     -> <op:u8> <item_id:u16_le> <n:u8>
  HAS_ANY_OF     -> <op:u8> <count:u8> <id:u16_le>×count
  HAS_ANY_COUNT  -> <op:u8> <count:u8> <id:u16_le>×count <n:u8>
  WORLDSTATE_EQ  -> <op:u8> <state:u8>
  GOAL_EQ        -> <op:u8> <goal:u8>
  GOAL_REQUIRES_DUNGEON -> <op:u8> <dungeon:u8>
  DUNGEON_CLEARED -> <op:u8> <dungeon:u8>
  REGION_REACHABLE -> <op:u8> <region_id:u16_le>
  HAS_PRIZE      -> <op:u8> <prize:u8>
  MEDALLION_OPENS -> <op:u8> <entrance:u8>
  ITEM_IS        -> <op:u8> <item_id:u16_le>
  NOT            -> <op:u8> <child>
  AND            -> <op:u8> <count:u8> <child>×count
  OR             -> <op:u8> <count:u8> <child>×count

  Vacuous AND (count=0) is TRUE; vacuous OR (count=0) is FALSE. The DSL
  literals TRUE() / FALSE() compile to those forms.

The well-formedness pass (task 3.10) asserts well-formedness conditions
listed in logic.schema.yaml `well_formedness:`.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import inspect
import json
import os
import re
import struct
import sys
from pathlib import Path

import yaml  # PyYAML; required by CLAUDE.md (`pip install -r requirements.txt`)

# Generated chest table + ALTTPR chest-name data (tasks.md S6.3).
# See assets/chest_data.py for the source-of-truth snapshot and provenance.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import chest_data  # noqa: E402  -- after sys.path mutation


# ---------------------------------------------------------------------------
# Atomic output write
# ---------------------------------------------------------------------------
def atomic_write_text(path: Path, text: str) -> None:
    """Write `text` to `path` ATOMICALLY: write a sibling temp file, flush+fsync,
    then os.replace() (an atomic rename on the same filesystem).

    Why this matters: the Makefile glob-builds these codegen outputs as ordinary
    sources, and under `make -j` a compile can read an output file. Path.write_text
    truncates-then-writes in place, so it leaves a window where the file is empty
    or partial — and a concurrent `gcc -c logic_data.c` (or a TU #including it)
    that reads during that window produces a SMALLER binary with a TRUNCATED logic
    graph (symptom: most corpus seeds fail with wrong placement digests; it is
    timing-dependent, so it masquerades as a real regression). With the atomic
    rename a reader sees EITHER the old complete file or the new complete file —
    never a mid-write one — so that race is structurally impossible. (Also defends
    against a double-codegen race: each writer renames a complete file, last wins.)

    newline is left at the default (None) to byte-for-byte match the previous
    Path.write_text(... encoding="utf-8") behavior on every platform (LF on POSIX,
    CRLF on Windows) — so emitted bytes, and thus digests, are unchanged.
    """
    text = "\n".join(line.rstrip() for line in text.splitlines()).rstrip() + "\n"
    if path.exists():
        try:
            if path.read_text(encoding="utf-8") == text:
                return
        except UnicodeDecodeError:
            pass
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
REPO = Path(__file__).resolve().parent.parent
RANDO_ASSETS = REPO / "assets" / "rando"
RANDO_SRC = REPO / "src" / "rando"
DUNGEON_SUFFIXES = [
    "HyruleCastleEscape", "EasternPalace", "DesertPalace", "TowerOfHera",
    "HyruleCastleTower", "PalaceOfDarkness", "SwampPalace", "SkullWoods",
    "ThievesTown", "IcePalace", "MiseryMire", "TurtleRock", "GanonsTower",
]

# Dungeon-chain logic factoring pool. Skull Woods, Castle Tower, Ganon's Tower,
# and Hyrule Castle stay outside v1 chains.
CHAIN_BOSS_ROOM_FACTORS = [
    ("EasternPalace", "Eastern Palace - Boss", "Eastern Palace - Prize"),
    ("DesertPalace", "Desert Palace - Boss", "Desert Palace - Prize"),
    ("TowerOfHera", "Tower of Hera - Boss", "Tower of Hera - Prize"),
    ("PalaceOfDarkness", "Palace of Darkness - Boss", "Palace of Darkness - Prize"),
    ("SwampPalace", "Swamp Palace - Boss", "Swamp Palace - Prize"),
    ("ThievesTown", "Thieves' Town - Boss", "Thieves' Town - Prize"),
    ("IcePalace", "Ice Palace - Boss", "Ice Palace - Prize"),
    ("MiseryMire", "Misery Mire - Boss", "Misery Mire - Prize"),
    ("TurtleRock", "Turtle Rock - Boss", "Turtle Rock - Prize"),
]
_CHAIN_BOSS_ROOM_REGION_IDS = [f"{dungeon}_BossRoom"
                               for dungeon, _boss, _prize in CHAIN_BOSS_ROOM_FACTORS]
_CHAIN_BOSS_ROOM_REGION_ID_SET = set(_CHAIN_BOSS_ROOM_REGION_IDS)


def _region_ids_for_codegen(regions: dict[str, "RegionDef"]) -> list[str]:
    """Return stable region IDs, preserving pre-chain numeric IDs.

    The historical table sorted region names alphabetically. Dungeon-chain boss
    rooms intentionally append after that baseline so dormant chains do not
    renumber existing entrance/door graph regions.
    """
    base = sorted(rid for rid in regions.keys()
                  if rid not in _CHAIN_BOSS_ROOM_REGION_ID_SET and
                  rid not in _OW_REGION_ID_SET)
    generated = [rid for rid in _CHAIN_BOSS_ROOM_REGION_IDS if rid in regions]
    # add-rando-ow-warp-shuffle: components + hub append LAST (after the
    # chain suffix) in load order, so every pre-existing region id is stable
    # (guarded by region_ids.lock.json) and the warp group is a contiguous
    # trailing suffix (kRandoOwRegionsFirst; inertness fast-path).
    ow = [rid for rid in _OW_REGION_IDS if rid in regions]
    return base + generated + ow


# ---------------------------------------------------------------------------
# Registry loading
# ---------------------------------------------------------------------------
@dataclasses.dataclass
class OpDef:
    id: int
    name: str
    phase: str
    operands: list
    contexts: list


@dataclasses.dataclass
class ItemDef:
    id: int
    name: str
    category: str
    display_name: str = ""
    max_count: int = 1
    dispatch: str = ""


@dataclasses.dataclass
class LocationDef:
    id: int
    name: str
    region: str
    type: str
    vanilla_item: str
    world_state_filter: list = dataclasses.field(default_factory=list)
    dungeon_item_class: str | None = None
    can_reach: str = "TRUE()"
    can_place: str = "TRUE()"
    always_allow: str = "FALSE()"
    source: str = ""


@dataclasses.dataclass(frozen=True)
class HintItemMetadata:
    item_id: int
    registry_name: str
    flags: int
    diversity_group: int
    short_name: str
    qualified_name: str
    compact_qualified_name: str


@dataclasses.dataclass(frozen=True)
class HintLocationMetadata:
    location_id: int
    registry_name: str
    compact_name: str


@dataclasses.dataclass(frozen=True)
class HintRegionMetadata:
    region_id: int
    registry_name: str
    qualified_name: str
    compact_name: str


@dataclasses.dataclass(frozen=True)
class HintMetadata:
    format_version: int
    algorithm_version: int
    text_schema_version: int
    algorithm_fingerprint: str
    text_fingerprint: str
    items: tuple[HintItemMetadata, ...]
    locations: tuple[HintLocationMetadata, ...]
    regions: tuple[HintRegionMetadata, ...]
    high_friction_location_ids: tuple[int, ...]


@dataclasses.dataclass
class MacroDef:
    name: str
    parameters: list
    body: str
    source: str = ""


@dataclasses.dataclass
class RegionDef:
    id: str
    name: str
    dungeon: str | None = None
    parent: str | None = None
    world_state_filter: list = dataclasses.field(default_factory=list)
    source: str = ""


@dataclasses.dataclass
class EdgeDef:
    from_: str
    to: str
    predicate: str
    one_way: bool = False
    source: str = ""


def load_yaml(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return yaml.safe_load(f)


def load_ops(path: Path) -> dict[str, OpDef]:
    doc = load_yaml(path)
    out = {}
    for raw in doc.get("ops", []):
        op = OpDef(
            id=raw["id"],
            name=raw["name"],
            phase=raw.get("phase", "A"),
            operands=raw.get("operands", []),
            contexts=raw.get("contexts", []),
        )
        # Strip the "OP_" prefix for DSL parser keys (DSL uses bare op names like HAS_ITEM)
        bare = op.name[3:] if op.name.startswith("OP_") else op.name
        out[bare] = op
    return out


def load_items(path: Path) -> dict[str, ItemDef]:
    doc = load_yaml(path)
    out = {}
    for raw in doc.get("items", []):
        it = ItemDef(
            id=raw["id"],
            name=raw["name"],
            category=raw.get("category", ""),
            display_name=raw.get("display_name", raw["name"]),
            max_count=raw.get("max_count", 1),
            dispatch=raw.get("dispatch", ""),
        )
        out[it.name] = it
    return out


def load_locations(path: Path) -> dict[str, LocationDef]:
    doc = load_yaml(path)
    out = {}
    for raw in doc.get("locations", []):
        loc = LocationDef(
            id=raw["id"],
            name=raw["name"],
            region=raw.get("region", ""),
            type=raw.get("type", "Chest"),
            vanilla_item=raw.get("vanilla_item", ""),
            world_state_filter=raw.get("world_state_filter", []),
            dungeon_item_class=raw.get("dungeon_item_class"),
            source=raw.get("source", ""),
        )
        # Map location.name -> a canonical id token usable in #define LOC_<id>.
        # We use the YAML location name lowered + non-alphanumerics replaced with _.
        out[loc.name] = loc
    return out


_HINT_ITEM_USEFUL = 0x01
_HINT_ITEM_MAJOR = 0x02
_HINT_ITEM_PRIORITY = 0x04
_HINT_ITEM_JUNK = 0x08


def _hint_mapping(value, label: str) -> dict:
    if not isinstance(value, dict):
        raise RuntimeError(f"hint metadata {label} must be a mapping")
    return value


def _hint_string_list(value, label: str) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(v, str) for v in value):
        raise RuntimeError(f"hint metadata {label} must be a list of strings")
    if len(set(value)) != len(value):
        raise RuntimeError(f"hint metadata {label} contains duplicate entries")
    return value


def _hint_split_identifier(value: str) -> str:
    """Turn a registry token into deterministic player-facing words.

    This generic rule is intentionally codegen-only. Runtime rendering consumes
    the emitted names, so adding a special spelling cannot create a second,
    hand-maintained C naming path.
    """
    value = value.replace("_", " ")
    value = re.sub(r"(?<=[a-z])(?=[A-Z])", " ", value)
    return re.sub(r"(?<=[A-Za-z])(?=[0-9])", " ", value)


def _hint_dungeon_identity(
    item_name: str,
    dungeon_kinds: dict[str, str],
) -> tuple[str, str] | None:
    """Return the registry dungeon-item kind and dungeon suffix, if present."""
    matches = [
        (prefix, item_name[len(prefix) + 1:])
        for prefix in dungeon_kinds
        if item_name.startswith(prefix + "_")
    ]
    if len(matches) > 1:
        raise RuntimeError(
            f"hint metadata item {item_name!r} matches multiple dungeon kinds "
            f"{sorted(prefix for prefix, _ in matches)}"
        )
    return matches[0] if matches else None


def _hint_item_names(
    item: ItemDef,
    overrides: dict,
    strip_prefixes: list[str],
    dungeon_kinds: dict[str, str],
    compact_dungeon_kinds: dict[str, str],
    dungeon_aliases: dict[str, str],
    compact_dungeon_aliases: dict[str, str],
) -> tuple[str, str, str, str | None]:
    raw = item.name
    dungeon_identity = _hint_dungeon_identity(raw, dungeon_kinds)
    override = overrides.get(raw)
    compact_override = None
    if override is not None:
        override = _hint_mapping(override, f"item_names.overrides.{raw}")
        unknown = set(override) - {
            "short",
            "qualified",
            "compact_qualified",
        }
        if unknown:
            raise RuntimeError(
                f"hint metadata item override {raw!r} has unknown keys "
                f"{sorted(unknown)}"
            )
        compact_override = override.get("compact_qualified")
        if compact_override is not None and (
            not isinstance(compact_override, str) or not compact_override
        ):
            raise RuntimeError(
                f"hint metadata item override {raw!r} has invalid "
                "compact_qualified name"
            )
        short = override.get("short")
        if short is None:
            if "qualified" in override:
                raise RuntimeError(
                    f"hint metadata item override {raw!r} cannot set qualified "
                    "without short"
                )
        elif not isinstance(short, str) or not short:
            raise RuntimeError(
                f"hint metadata item override {raw!r} needs a non-empty short name"
            )
        if short is not None:
            qualified = override.get("qualified", short)
            compact_qualified = compact_override or qualified
            if not isinstance(qualified, str) or not qualified:
                raise RuntimeError(
                    f"hint metadata item override {raw!r} has invalid "
                    "qualified name"
                )
            return (
                short,
                qualified,
                compact_qualified,
                dungeon_identity[0] if dungeon_identity is not None else None,
            )
        if compact_override is None:
            raise RuntimeError(
                f"hint metadata item override {raw!r} has no naming field"
            )

    if dungeon_identity is not None:
        prefix, suffix = dungeon_identity
        kind_name = dungeon_kinds[prefix]
        dungeon_name = dungeon_aliases.get(suffix)
        compact_dungeon_name = compact_dungeon_aliases.get(suffix)
        if dungeon_name is None:
            raise RuntimeError(
                f"hint metadata item {raw!r} has unknown dungeon suffix "
                f"{suffix!r}; add a dungeon_aliases entry"
            )
        if compact_dungeon_name is None:
            raise RuntimeError(
                f"hint metadata item {raw!r} has no compact dungeon alias for "
                f"{suffix!r}"
            )
        compact_kind_name = compact_dungeon_kinds[prefix]
        return (
            kind_name,
            f"{dungeon_name} {kind_name}",
            compact_override or f"{compact_dungeon_name} {compact_kind_name}",
            prefix,
        )

    if item.category == "dungeon_item":
        raise RuntimeError(
            f"hint metadata dungeon item {raw!r} has no recognized kind; "
            "add a dungeon_kinds entry or an explicit item_names override"
        )

    if raw.startswith("Prize_Crystal"):
        short = "Crystal " + raw[len("Prize_Crystal"):]
        return short, short, compact_override or short, None
    if raw.startswith("Soul_Npc_"):
        short = _hint_split_identifier(raw[len("Soul_Npc_"):]) + " Soul"
        return (
            short,
            short,
            compact_override
            or "S-" + _hint_split_identifier(raw[len("Soul_Npc_"):]),
            None,
        )
    if raw.startswith("Soul_"):
        short = _hint_split_identifier(raw[len("Soul_"):]) + " Soul"
        return (
            short,
            short,
            compact_override
            or "S-" + _hint_split_identifier(raw[len("Soul_"):]),
            None,
        )
    quantity = re.fullmatch(r"(Rupee|Arrow|Bombs)(\d+)", raw)
    if quantity is not None:
        family, raw_count = quantity.groups()
        count = int(raw_count)
        noun = {
            "Rupee": "Rupee" if count == 1 else "Rupees",
            "Arrow": "Arrow" if count == 1 else "Arrows",
            "Bombs": "Bomb" if count == 1 else "Bombs",
        }[family]
        short = f"{count} {noun}"
        return short, short, compact_override or short, None
    if raw.startswith("Trap"):
        adjective = _hint_split_identifier(raw[len("Trap"):])
        if adjective == "Fake Low Hp":
            adjective = "Fake Low HP"
        short = f"{adjective} Trap"
        return short, short, compact_override or short, None

    friendly = raw
    for prefix in strip_prefixes:
        if friendly.startswith(prefix):
            friendly = friendly[len(prefix):]
            break
    friendly = _hint_split_identifier(friendly)
    friendly = friendly.replace(" Of ", " of ").replace(" With ", " with ")
    return friendly, friendly, compact_override or friendly, None


def _hint_classify_item(
    item: ItemDef,
    dungeon_kind: str | None,
    category_owner: dict[str, str],
    item_overrides: dict[str, str],
    major_dungeon_kinds: set[str],
    priority_items: set[str],
) -> int:
    """Return generated eligibility flags for one registry item."""
    hint_class = item_overrides.get(
        item.name, category_owner.get(item.category)
    )
    if dungeon_kind in major_dungeon_kinds:
        hint_class = "major"
    if hint_class is None:
        raise RuntimeError(
            f"hint metadata leaves item {item.name!r} "
            f"(category {item.category!r}) unclassified"
        )
    if hint_class == "junk":
        flags = _HINT_ITEM_JUNK
    elif hint_class == "major":
        flags = _HINT_ITEM_USEFUL | _HINT_ITEM_MAJOR
    else:
        flags = _HINT_ITEM_USEFUL
    if item.name in priority_items:
        if hint_class != "major":
            raise RuntimeError(
                f"hint metadata priority item {item.name!r} is not major"
            )
        flags |= _HINT_ITEM_PRIORITY
    return flags


def _hint_compact_location_name(
    name: str,
    explicit_aliases: dict[str, str],
    base_aliases: dict[str, str],
) -> str:
    explicit = explicit_aliases.get(name)
    if explicit is not None:
        return explicit

    # Generated ordinary-enemy checks carry stable physical coordinates. Keep
    # those coordinates and discard only the descriptive enemy suffix.
    match = re.fullmatch(
        r"Enemy Check - Overworld 0x([0-9A-Fa-f]+) "
        r"Stage (\d+) Slot (\d+)(?: - .*)?",
        name,
    )
    if match:
        return (
            f"Enemy O{int(match.group(1), 16):02X} "
            f"T{int(match.group(2))} S{int(match.group(3)):02d}"
        )
    match = re.fullmatch(
        r"Enemy Check - Room 0x([0-9A-Fa-f]+) Slot (\d+)(?: - .*)?",
        name,
    )
    if match:
        return (
            f"Enemy R{int(match.group(1), 16):03X} "
            f"S{int(match.group(2)):02d}"
        )
    match = re.fullmatch(
        r"Enemy Check - Room 0x([0-9A-Fa-f]+) "
        r"Script (\d+) Child (\d+)(?: - .*)?",
        name,
    )
    if match:
        return (
            f"Enemy R{int(match.group(1), 16):03X} "
            f"X{int(match.group(2)):02d} C{int(match.group(3))}"
        )

    for base, alias in sorted(
        base_aliases.items(), key=lambda pair: len(pair[0]), reverse=True
    ):
        if name == base:
            return alias
        suffix_marker = base + " - "
        if name.startswith(suffix_marker):
            # Preserve the complete semantic suffix, including Left/Right and
            # shop/take-any slot numbers.
            return alias + name[len(base):]

    tokens = re.findall(r"[^ _()/\-]+", name)
    if not tokens:
        raise RuntimeError(f"hint metadata cannot compact empty location {name!r}")
    if len(tokens) <= 2:
        return " ".join(tokens)

    # Acronymize only the prefix. The final TWO semantic tokens survive in
    # full, and any earlier digit-bearing coordinate token survives too.
    #   DarkWorld_NorthWest Bush S52 P05AE -> DWNWB S52 P05AE
    prefix_out: list[str] = []
    acronym = ""
    for token in tokens[:-2]:
        if any(ch.isdigit() for ch in token):
            if acronym:
                prefix_out.append(acronym)
                acronym = ""
            prefix_out.append(token)
            continue
        capitals = "".join(ch for ch in token if ch.isupper())
        acronym += capitals or token[0].upper()
    if acronym:
        prefix_out.append(acronym)
    return " ".join([*prefix_out, *tokens[-2:]])


def _hint_encode_game_identity(text: str) -> bytes | None:
    """Mirror the runtime US encoder, including glyphless-char substitution."""
    def glyph(character: str) -> int:
        if "A" <= character <= "Z":
            return ord(character) - ord("A")
        if "a" <= character <= "z":
            return 26 + ord(character) - ord("a")
        if "0" <= character <= "9":
            return 52 + ord(character) - ord("0")
        return {
            "!": 62,
            "?": 63,
            "-": 64,
            ".": 65,
            ",": 66,
            ">": 68,
            "'": 81,
            " ": 89,
        }.get(character, 89)

    encoded = bytearray()
    row = 0
    column = 0
    for word in text.split():
        if column and column + 1 + len(word) > 13:
            row += 1
            column = 0
            if row >= 3:
                return None
            encoded.append(0x74 + row)
        elif column:
            column += 1
            encoded.append(glyph(" "))
        for _ in word:
            if column >= 13:
                row += 1
                column = 0
                if row >= 3:
                    return None
                encoded.append(0x74 + row)
            encoded.append(glyph(_))
            column += 1
    return bytes(encoded)


def _hint_text_fits_game(text: str) -> bool:
    """Mirror the runtime's conservative 3 rows x 13 glyph wrap contract."""
    return _hint_encode_game_identity(text) is not None


def _hint_compact_region_name(
    region_id: str,
    display_name: str,
    explicit_aliases: dict[str, str],
) -> str:
    """Create a readable, stable region label for the count-fact envelope."""
    explicit = explicit_aliases.get(region_id)
    if explicit is not None:
        return explicit

    compact = display_name.replace("OW: ", "OW ").replace(" - ", " ")
    phrase_replacements = (
        ("Light World", "LW"),
        ("Dark World", "DW"),
        ("Death Mountain", "DM"),
        ("Hyrule Castle", "HC"),
        ("Palace of Darkness", "PoD"),
        ("Eastern Palace", "Eastern"),
        ("Desert Palace", "Desert"),
        ("Swamp Palace", "Swamp"),
        ("Ice Palace", "Ice"),
        ("Turtle Rock", "TR"),
        ("Misery Mire", "Mire"),
        ("Tower of Hera", "Hera"),
        ("Ganons Tower", "GT"),
    )
    for old, new in phrase_replacements:
        compact = compact.replace(old, new)
    word_replacements = {
        "Northeast": "NE",
        "Northwest": "NW",
        "Southeast": "SE",
        "Southwest": "SW",
        "North": "N",
        "South": "S",
        "East": "E",
        "West": "W",
        "Bottom": "Bot",
        "Courtyard": "Court",
        "Entrance": "Entry",
        "Floating": "Float",
        "Isolated": "Isle",
        "Mountain": "Mt",
        "Portal": "Port",
        "Waterfall": "Falls",
        "Whirlpool": "Whirl",
    }
    compact = " ".join(
        word_replacements.get(word, word) for word in compact.split()
    )
    if len(compact) <= 20:
        return compact
    # Preserve the two most specific semantic words; acronymize only the broad
    # prefix. Any collision is a fatal request for an explicit alias.
    return _hint_compact_location_name(compact, {}, {})


def _hint_runtime_redirect_item_names(path: Path) -> set[str]:
    source = path.read_text(encoding="utf-8")
    table = re.search(
        r"kDialogueHintRedirects\[\]\s*=\s*\{(.*?)\n\};",
        source,
        flags=re.DOTALL,
    )
    if table is None:
        raise RuntimeError(
            f"cannot parse kDialogueHintRedirects from {path}"
        )
    names: set[str] = set()
    for row in re.finditer(r"\{(.*?)\}", table.group(1), flags=re.DOTALL):
        text = row.group(1)
        if "kDialogueHintRedirect_LocationItem" in text:
            continue
        item = re.search(r"\bITEM_([A-Za-z0-9_]+)\b", text)
        if item is None:
            raise RuntimeError(
                f"cannot parse item from redirect row {text!r}"
            )
        names.add(item.group(1))
    return names


def _hint_json_fingerprint(value) -> str:
    return hashlib.sha256(
        json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("ascii")
    ).hexdigest()


def _hint_registry_contract(
    locations: dict[str, LocationDef],
    regions: dict[str, RegionDef],
    region_ids: list[str],
    world_state_overrides: dict[str, dict[int, LocationDef]],
    *,
    base_location_count: int,
    base_region_count: int,
) -> dict:
    sorted_locations = sorted(locations.values(), key=lambda loc: loc.id)
    world_state_rows = sorted(
        (
            str(location_key),
            int(world_state),
            override.id,
            override.name,
            override.region,
            override.type,
        )
        for location_key, world_states in world_state_overrides.items()
        for world_state, override in world_states.items()
    )
    type_counts: dict[str, int] = {}
    for location in sorted_locations:
        type_counts[location.type] = type_counts.get(location.type, 0) + 1
    algorithm_rows = {
        "locations": [
            (loc.id, loc.name, loc.region, loc.type)
            for loc in sorted_locations
        ],
        "regions": [
            (
                index,
                region_id,
                regions[region_id].parent,
                regions[region_id].world_state_filter,
            )
            for index, region_id in enumerate(region_ids)
        ],
        "world_state_region_overrides": world_state_rows,
    }
    text_rows = {
        "locations": [(loc.id, loc.name) for loc in sorted_locations],
        "regions": [
            (index, region_id, regions[region_id].name)
            for index, region_id in enumerate(region_ids)
        ],
    }
    provenance_rows = {
        "locations": [
            (loc.id, loc.source) for loc in sorted_locations
        ],
        "regions": [
            (index, region_id, regions[region_id].source)
            for index, region_id in enumerate(region_ids)
        ],
    }
    return {
        "format_version": 1,
        "base_location_count": base_location_count,
        "base_region_count": base_region_count,
        "location_count": len(sorted_locations),
        "region_count": len(region_ids),
        "location_type_counts": dict(sorted(type_counts.items())),
        "algorithm_registry_fingerprint": _hint_json_fingerprint(
            algorithm_rows
        ),
        "text_registry_fingerprint": _hint_json_fingerprint(text_rows),
        "provenance_fingerprint": _hint_json_fingerprint(provenance_rows),
    }


def load_hint_registry_contract(path: Path) -> dict:
    expected_keys = {
        "format_version",
        "base_location_count",
        "base_region_count",
        "location_count",
        "region_count",
        "location_type_counts",
        "algorithm_registry_fingerprint",
        "text_registry_fingerprint",
        "provenance_fingerprint",
    }
    if not path.exists():
        raise RuntimeError(
            f"{path} is missing; regenerate intentionally with "
            "--update-hint-metadata-lock"
        )
    contract = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(contract, dict) or set(contract) != expected_keys:
        raise RuntimeError(
            f"{path} has invalid schema; expected exactly "
            f"{sorted(expected_keys)}"
        )
    for key in (
        "algorithm_registry_fingerprint",
        "text_registry_fingerprint",
        "provenance_fingerprint",
    ):
        if not re.fullmatch(r"[0-9a-f]{64}", str(contract.get(key, ""))):
            raise RuntimeError(f"{path} has invalid {key}")
    return contract


def validate_hint_registry_contract(
    path: Path,
    locations: dict[str, LocationDef],
    regions: dict[str, RegionDef],
    region_ids: list[str],
    world_state_overrides: dict[str, dict[int, LocationDef]],
    *,
    base_location_count: int,
    base_region_count: int,
    update: bool = False,
) -> dict:
    actual = _hint_registry_contract(
        locations,
        regions,
        region_ids,
        world_state_overrides,
        base_location_count=base_location_count,
        base_region_count=base_region_count,
    )
    if update:
        required_artifacts = (
            RANDO_ASSETS / "pots.gen.yaml",
            RANDO_ASSETS / "enemy_drops.gen.yaml",
            RANDO_ASSETS / "enemy_checks.gen.yaml",
            RANDO_ASSETS / "terrain.gen.yaml",
            RANDO_ASSETS / "bonk.gen.yaml",
            RANDO_ASSETS / "ow_graph.gen.yaml",
        )
        missing_artifacts = [
            str(artifact)
            for artifact in required_artifacts
            if not artifact.exists()
        ]
        required_types = {
            "Pot",
            "EnemyDrop",
            "Enemy",
            "Grass",
            "Rock",
            "Bonk",
        }
        missing_types = required_types - set(
            actual["location_type_counts"]
        )
        if (
            actual["location_count"] <= base_location_count
            or actual["region_count"] <= base_region_count
            or missing_artifacts
            or missing_types
        ):
            raise RuntimeError(
                "refusing to approve an assetless/partial Hints merged-registry "
                f"contract: missing artifacts={missing_artifacts}, "
                f"missing location types={sorted(missing_types)}"
            )
        return actual

    approved = load_hint_registry_contract(path)
    if actual == approved:
        return approved
    if (
        actual["location_count"] == approved["base_location_count"]
        and actual["region_count"] == approved["base_region_count"]
    ):
        # Public/assetless builds still bind their metadata fingerprints to the
        # approved shipped superset. Base registry rows are independently
        # included verbatim in those fingerprints below.
        return approved
    raise RuntimeError(
        "hint merged-registry contract drift/partial artifact set: "
        f"approved locations={approved['location_count']} "
        f"regions={approved['region_count']}, actual "
        f"locations={actual['location_count']} regions={actual['region_count']}"
    )


def load_hint_metadata(
    path: Path,
    items: dict[str, ItemDef],
    locations: dict[str, LocationDef],
    contract_locations: dict[str, LocationDef] | None = None,
    regions: dict[str, RegionDef] | None = None,
    region_ids: list[str] | None = None,
    contract_regions: dict[str, RegionDef] | None = None,
    contract_region_ids: list[str] | None = None,
    contract_world_state_overrides:
        dict[str, dict[int, LocationDef]] | None = None,
    registry_contract: dict | None = None,
) -> HintMetadata:
    """Load and validate the one registry-tied Hints v2 metadata source.

    Validation is deliberately fatal during ordinary codegen, not just under
    --strict: an unknown item/dungeon identity or a lossy compact collision
    makes a rich fact semantically unsafe.
    """
    doc = load_yaml(path)
    if not isinstance(doc, dict):
        raise RuntimeError(f"{path} must contain a mapping")
    if doc.get("format_version") != 1:
        raise RuntimeError(
            f"{path} format_version must be 1, got {doc.get('format_version')!r}"
        )
    if registry_contract is None:
        raise RuntimeError(
            "hint metadata requires the approved merged-registry contract"
        )
    if registry_contract.get("format_version") != 1:
        raise RuntimeError("hint merged-registry contract format drift")
    algorithm_version = doc.get("algorithm_version")
    if not isinstance(algorithm_version, int) or not (
        1 <= algorithm_version <= 0xFFFF
    ):
        raise RuntimeError(
            f"{path} algorithm_version must be an integer in 1..65535"
        )
    text_schema_version = doc.get("text_schema_version")
    if not isinstance(text_schema_version, int) or not (
        1 <= text_schema_version <= 0xFFFF
    ):
        raise RuntimeError(
            f"{path} text_schema_version must be an integer in 1..65535"
        )
    expected_top = {
        "format_version",
        "algorithm_version",
        "text_schema_version",
        "item_classification",
        "diversity_groups",
        "item_names",
        "dungeon_aliases",
        "compact_dungeon_aliases",
        "location_base_aliases",
        "location_aliases",
        "region_aliases",
        "redirect_item_location_items",
        "high_friction_locations",
    }
    unknown_top = set(doc) - expected_top
    if unknown_top:
        raise RuntimeError(
            f"{path} has unknown top-level keys {sorted(unknown_top)}"
        )

    classification = _hint_mapping(
        doc.get("item_classification"), "item_classification"
    )
    expected_classification = {
        "junk_registry_categories",
        "major_registry_categories",
        "useful_registry_categories",
        "major_dungeon_kinds",
        "item_overrides",
        "priority_items",
    }
    unknown_classification = set(classification) - expected_classification
    missing_classification = expected_classification - set(classification)
    if unknown_classification or missing_classification:
        raise RuntimeError(
            "hint metadata item_classification key drift: "
            f"missing={sorted(missing_classification)} "
            f"unknown={sorted(unknown_classification)}"
        )

    class_categories = {
        "junk": _hint_string_list(
            classification["junk_registry_categories"],
            "item_classification.junk_registry_categories",
        ),
        "major": _hint_string_list(
            classification["major_registry_categories"],
            "item_classification.major_registry_categories",
        ),
        "useful": _hint_string_list(
            classification["useful_registry_categories"],
            "item_classification.useful_registry_categories",
        ),
    }
    category_owner: dict[str, str] = {}
    registry_categories = {item.category for item in items.values()}
    for hint_class, categories in class_categories.items():
        for category in categories:
            if category not in registry_categories:
                raise RuntimeError(
                    f"hint metadata {hint_class} selector references unknown "
                    f"registry category {category!r}"
                )
            prior = category_owner.get(category)
            if prior is not None:
                raise RuntimeError(
                    f"hint metadata registry category {category!r} belongs to "
                    f"both {prior} and {hint_class}"
                )
            category_owner[category] = hint_class

    item_overrides = _hint_mapping(
        classification["item_overrides"],
        "item_classification.item_overrides",
    )
    for name, hint_class in item_overrides.items():
        if name not in items:
            raise RuntimeError(
                f"hint metadata classification references unknown item {name!r}"
            )
        if hint_class not in {"junk", "major", "useful"}:
            raise RuntimeError(
                f"hint metadata item override {name!r} has invalid class "
                f"{hint_class!r}"
            )

    item_names = _hint_mapping(doc.get("item_names"), "item_names")
    expected_item_names = {
        "overrides",
        "strip_prefixes",
        "dungeon_kinds",
        "compact_dungeon_kinds",
    }
    if set(item_names) != expected_item_names:
        raise RuntimeError(
            "hint metadata item_names key drift: "
            f"expected={sorted(expected_item_names)} actual={sorted(item_names)}"
        )
    name_overrides = _hint_mapping(
        item_names["overrides"], "item_names.overrides"
    )
    for name in name_overrides:
        if name not in items:
            raise RuntimeError(
                f"hint metadata naming references unknown item {name!r}"
            )
    strip_prefixes = _hint_string_list(
        item_names["strip_prefixes"], "item_names.strip_prefixes"
    )
    dungeon_kinds = _hint_mapping(
        item_names["dungeon_kinds"], "item_names.dungeon_kinds"
    )
    if any(
        not isinstance(key, str)
        or not key
        or not isinstance(value, str)
        or not value
        for key, value in dungeon_kinds.items()
    ):
        raise RuntimeError("hint metadata dungeon_kinds must map strings to strings")
    compact_dungeon_kinds = _hint_mapping(
        item_names["compact_dungeon_kinds"],
        "item_names.compact_dungeon_kinds",
    )
    if (
        set(compact_dungeon_kinds) != set(dungeon_kinds)
        or any(
            not isinstance(value, str) or not value
            for value in compact_dungeon_kinds.values()
        )
    ):
        raise RuntimeError(
            "hint metadata compact_dungeon_kinds must map every dungeon kind "
            "to a non-empty string"
        )
    dungeon_aliases = _hint_mapping(
        doc.get("dungeon_aliases"), "dungeon_aliases"
    )
    if any(
        not isinstance(key, str)
        or not key
        or not isinstance(value, str)
        or not value
        for key, value in dungeon_aliases.items()
    ):
        raise RuntimeError(
            "hint metadata dungeon_aliases must map strings to strings"
        )
    compact_dungeon_aliases = _hint_mapping(
        doc.get("compact_dungeon_aliases"), "compact_dungeon_aliases"
    )
    if (
        set(compact_dungeon_aliases) != set(dungeon_aliases)
        or any(
            not isinstance(value, str) or not value
            for value in compact_dungeon_aliases.values()
        )
    ):
        raise RuntimeError(
            "hint metadata compact_dungeon_aliases must map every dungeon "
            "suffix to a non-empty string"
        )

    major_dungeon_kinds = set(
        _hint_string_list(
            classification["major_dungeon_kinds"],
            "item_classification.major_dungeon_kinds",
        )
    )
    unknown_major_kinds = major_dungeon_kinds - set(dungeon_kinds)
    if unknown_major_kinds:
        raise RuntimeError(
            "hint metadata major_dungeon_kinds references unknown naming kinds "
            f"{sorted(unknown_major_kinds)}"
        )
    priority_items = set(
        _hint_string_list(
            classification["priority_items"],
            "item_classification.priority_items",
        )
    )
    unknown_priority = priority_items - set(items)
    if unknown_priority:
        raise RuntimeError(
            f"hint metadata priority_items references unknown items "
            f"{sorted(unknown_priority)}"
        )

    raw_diversity_groups = doc.get("diversity_groups")
    if not isinstance(raw_diversity_groups, list):
        raise RuntimeError("hint metadata diversity_groups must be a list")
    diversity_group_by_item: dict[str, int] = {}
    diversity_group_ids: set[int] = set()
    diversity_group_names: set[str] = set()
    for index, raw_group in enumerate(raw_diversity_groups):
        group = _hint_mapping(raw_group, f"diversity_groups[{index}]")
        if set(group) != {"id", "name", "items"}:
            raise RuntimeError(
                f"hint metadata diversity_groups[{index}] must contain exactly "
                "id, name, and items"
            )
        group_id = group["id"]
        group_name = group["name"]
        if not isinstance(group_id, int) or not 1 <= group_id <= 0xFF:
            raise RuntimeError(
                f"hint metadata diversity group id {group_id!r} is not 1..255"
            )
        if group_id in diversity_group_ids:
            raise RuntimeError(
                f"hint metadata diversity group id {group_id} is duplicated"
            )
        if not isinstance(group_name, str) or not group_name:
            raise RuntimeError(
                f"hint metadata diversity group {group_id} has invalid name"
            )
        if group_name in diversity_group_names:
            raise RuntimeError(
                f"hint metadata diversity group name {group_name!r} is duplicated"
            )
        group_items = _hint_string_list(
            group["items"], f"diversity_groups[{index}].items"
        )
        if len(group_items) < 2:
            raise RuntimeError(
                f"hint metadata diversity group {group_name!r} needs at least "
                "two items"
            )
        for item_name in group_items:
            if item_name not in items:
                raise RuntimeError(
                    f"hint metadata diversity group {group_name!r} references "
                    f"unknown item {item_name!r}"
                )
            prior_group = diversity_group_by_item.get(item_name)
            if prior_group is not None:
                raise RuntimeError(
                    f"hint metadata item {item_name!r} belongs to diversity "
                    f"groups {prior_group} and {group_id}"
                )
            diversity_group_by_item[item_name] = group_id
        diversity_group_ids.add(group_id)
        diversity_group_names.add(group_name)

    sorted_items = sorted(items.values(), key=lambda item: item.id)
    if [item.id for item in sorted_items] != list(range(len(sorted_items))):
        raise RuntimeError(
            "hint metadata requires the item registry to remain dense from id 0"
        )
    emitted_items: list[HintItemMetadata] = []
    used_dungeon_kinds: set[str] = set()
    for item in sorted_items:
        (
            short_name,
            qualified_name,
            compact_qualified_name,
            dungeon_kind,
        ) = _hint_item_names(
            item,
            name_overrides,
            strip_prefixes,
            dungeon_kinds,
            compact_dungeon_kinds,
            dungeon_aliases,
            compact_dungeon_aliases,
        )
        if dungeon_kind is not None:
            used_dungeon_kinds.add(dungeon_kind)

        flags = _hint_classify_item(
            item,
            dungeon_kind,
            category_owner,
            item_overrides,
            major_dungeon_kinds,
            priority_items,
        )
        for label, value in (
            ("short", short_name),
            ("qualified", qualified_name),
            ("compact-qualified", compact_qualified_name),
        ):
            if not value or len(value.encode("ascii", errors="strict")) >= 64:
                raise RuntimeError(
                    f"hint metadata item {item.name!r} {label} name "
                    "is empty, non-ASCII, or exceeds the 63-byte runtime buffer"
                )
            if " at " in value:
                raise RuntimeError(
                    f"hint metadata item {item.name!r} {label} name contains "
                    "the exact-template delimiter ' at '"
                )
            if value != " ".join(value.split()):
                raise RuntimeError(
                    f"hint metadata item {item.name!r} {label} name contains "
                    "leading, trailing, or repeated whitespace"
                )
        emitted_items.append(
            HintItemMetadata(
                item.id,
                item.name,
                flags,
                diversity_group_by_item.get(item.name, 0),
                short_name,
                qualified_name,
                compact_qualified_name,
            )
        )

    unused_kinds = set(dungeon_kinds) - used_dungeon_kinds
    if unused_kinds:
        raise RuntimeError(
            f"hint metadata dungeon_kinds has unused entries {sorted(unused_kinds)}"
        )

    explicit_location_aliases = _hint_mapping(
        doc.get("location_aliases"), "location_aliases"
    )
    base_location_aliases = _hint_mapping(
        doc.get("location_base_aliases"), "location_base_aliases"
    )
    for label, aliases in (
        ("location_aliases", explicit_location_aliases),
        ("location_base_aliases", base_location_aliases),
    ):
        if any(
            not isinstance(key, str)
            or not key
            or not isinstance(value, str)
            or not value
            for key, value in aliases.items()
        ):
            raise RuntimeError(
                f"hint metadata {label} must map non-empty strings to strings"
            )

    location_names = set(locations)
    unknown_location_aliases = set(explicit_location_aliases) - location_names
    if unknown_location_aliases:
        raise RuntimeError(
            "hint metadata location_aliases references unknown locations "
            f"{sorted(unknown_location_aliases)}"
        )
    for base in base_location_aliases:
        if not any(
            name == base or name.startswith(base + " - ")
            for name in location_names
        ):
            raise RuntimeError(
                f"hint metadata base location alias {base!r} matches no registry "
                "location"
            )

    sorted_locations = sorted(locations.values(), key=lambda loc: loc.id)
    if len({loc.id for loc in sorted_locations}) != len(sorted_locations):
        raise RuntimeError("hint metadata location ids are not unique")
    if len({loc.name for loc in sorted_locations}) != len(sorted_locations):
        raise RuntimeError("hint metadata location registry names are not unique")
    emitted_locations: list[HintLocationMetadata] = []
    compact_owner: dict[str, HintLocationMetadata] = {}
    for location in sorted_locations:
        compact_name = _hint_compact_location_name(
            location.name,
            explicit_location_aliases,
            base_location_aliases,
        )
        if (
            not compact_name
            or len(compact_name.encode("ascii", errors="strict")) >= 64
        ):
            raise RuntimeError(
                f"hint metadata compact alias for {location.name!r} is empty, "
                "non-ASCII, or exceeds the 63-byte runtime buffer"
            )
        if " at " in compact_name:
            raise RuntimeError(
                f"hint metadata compact alias for {location.name!r} contains "
                "the exact-template delimiter ' at '"
            )
        if compact_name != " ".join(compact_name.split()):
            raise RuntimeError(
                f"hint metadata compact alias for {location.name!r} contains "
                "leading, trailing, or repeated whitespace"
            )
        row = HintLocationMetadata(location.id, location.name, compact_name)
        prior = compact_owner.get(compact_name)
        if prior is not None:
            raise RuntimeError(
                "hint metadata lossy compact-location collision: "
                f"{prior.location_id} ({prior.registry_name!r}) and "
                f"{location.id} ({location.name!r}) both become "
                f"{compact_name!r}; add explicit location_aliases"
            )
        compact_owner[compact_name] = row
        emitted_locations.append(row)

    # Exact rich facts retain qualified prose in the semantic/spoiler field and
    # use a separate lossless compact-qualified item name only when the in-game
    # three-row renderer needs the compact location form.
    useful_items = [
        item for item in emitted_items if item.flags & _HINT_ITEM_USEFUL
    ]
    qualified_owner: dict[str, HintItemMetadata] = {}
    compact_item_owner: dict[str, HintItemMetadata] = {}
    reward_identity_owner: dict[bytes, HintItemMetadata] = {}
    for item in emitted_items:
        prior = qualified_owner.get(item.qualified_name)
        if prior is not None:
            raise RuntimeError(
                "hint metadata reward-preview exact-text collision: items "
                f"{prior.registry_name!r} and {item.registry_name!r} share "
                f"qualified name {item.qualified_name!r}"
            )
        qualified_owner[item.qualified_name] = item
        prior = compact_item_owner.get(item.compact_qualified_name)
        if prior is not None:
            raise RuntimeError(
                "hint metadata reward-preview compact-qualified collision: items "
                f"{prior.registry_name!r} and {item.registry_name!r} share "
                f"{item.compact_qualified_name!r}"
            )
        if item.compact_qualified_name.endswith(" is"):
            raise RuntimeError(
                f"hint metadata compact-qualified item {item.registry_name!r} "
                "must not end in ' is' (it would cross-collide with the full "
                "exact template)"
            )
        compact_item_owner[item.compact_qualified_name] = item
        for label, value in (
            ("qualified", item.qualified_name),
            ("compact-qualified", item.compact_qualified_name),
        ):
            identity = _hint_encode_game_identity(value)
            if identity is None:
                raise RuntimeError(
                    f"hint metadata reward-preview {label} item "
                    f"{item.registry_name!r} cannot render"
                )
            prior = reward_identity_owner.get(identity)
            if prior is not None and prior.item_id != item.item_id:
                raise RuntimeError(
                    "hint metadata reward-preview encoded-glyph collision: "
                    f"items {prior.registry_name!r} and "
                    f"{item.registry_name!r} share {value!r}"
                )
            reward_identity_owner[identity] = item
    expected_exact_identities = len(useful_items) * len(emitted_locations)
    proven_exact_identities = len(useful_items) * len(compact_owner)
    if proven_exact_identities != expected_exact_identities:
        raise RuntimeError(
            "hint metadata final exact-text uniqueness proof failed: "
            f"expected {expected_exact_identities}, proved "
            f"{proven_exact_identities}"
        )

    fit_failures: list[str] = []
    fit_failure_count = 0
    exact_game_identities: set[bytes] = set()
    for item in useful_items:
        for location in emitted_locations:
            full = (
                f"{item.qualified_name} is at {location.registry_name}"
            )
            compact = (
                f"{item.compact_qualified_name} at {location.compact_name}"
            )
            full_identity = _hint_encode_game_identity(full)
            compact_identity = _hint_encode_game_identity(compact)
            if len(full.encode("ascii")) >= 160 or compact_identity is None:
                fit_failure_count += 1
                if len(fit_failures) < 8:
                    fit_failures.append(
                        f"{item.registry_name}@{location.location_id}: "
                        f"full={len(full)} compact={compact!r}"
                    )
                continue
            final_text = full if full_identity is not None else compact
            if len(final_text.encode("ascii")) >= 96:
                fit_failure_count += 1
                if len(fit_failures) < 8:
                    fit_failures.append(
                        f"{item.registry_name}@{location.location_id}: "
                        f"chosen game text is {len(final_text)} bytes"
                    )
                continue
            final_identity = full_identity or compact_identity
            identity_digest = hashlib.blake2b(
                final_identity, digest_size=16
            ).digest()
            if identity_digest in exact_game_identities:
                raise RuntimeError(
                    "hint metadata final encoded-glyph exact identity "
                    f"collides at {item.registry_name!r}/"
                    f"{location.registry_name!r}; author lossless aliases "
                    "without glyphless punctuation"
                )
            exact_game_identities.add(identity_digest)
    if fit_failure_count:
        raise RuntimeError(
            "hint metadata exact-render envelope leaves "
            f"{fit_failure_count}/{expected_exact_identities} useful "
            f"item/location pairs unhintable; examples={fit_failures}"
        )
    del exact_game_identities

    redirect_item_names = _hint_string_list(
        doc.get("redirect_item_location_items"),
        "redirect_item_location_items",
    )
    unknown_redirect_items = set(redirect_item_names) - set(items)
    if unknown_redirect_items:
        raise RuntimeError(
            "hint metadata redirect_item_location_items references unknown "
            f"items {sorted(unknown_redirect_items)}"
        )
    runtime_redirect_items = _hint_runtime_redirect_item_names(
        RANDO_SRC / "rando_hints.c"
    )
    if runtime_redirect_items != set(redirect_item_names):
        raise RuntimeError(
            "hint metadata redirect item drift: YAML="
            f"{sorted(redirect_item_names)} runtime="
            f"{sorted(runtime_redirect_items)}"
        )
    emitted_item_by_name = {
        item.registry_name: item for item in emitted_items
    }
    redirect_short_identities: dict[bytes, str] = {}
    for item_name in redirect_item_names:
        identity = _hint_encode_game_identity(
            emitted_item_by_name[item_name].short_name
        )
        if identity is None:
            raise RuntimeError(
                f"hint metadata redirect short name {item_name!r} cannot render"
            )
        prior = redirect_short_identities.get(identity)
        if prior is not None:
            raise RuntimeError(
                "hint metadata encoded redirect short-name collision: "
                f"{prior!r} and {item_name!r}"
            )
        redirect_short_identities[identity] = item_name
    redirect_failures: list[str] = []
    redirect_failure_count = 0
    for item_name in redirect_item_names:
        item = emitted_item_by_name[item_name]
        for location in emitted_locations:
            candidates = (
                f"{item.short_name} is in {location.registry_name}",
                f"{item.short_name} at {location.registry_name}",
                f"{item.short_name} at {location.compact_name}",
            )
            if not any(_hint_text_fits_game(text) for text in candidates):
                redirect_failure_count += 1
                if len(redirect_failures) < 8:
                    redirect_failures.append(
                        f"{item_name}@{location.location_id}"
                    )
    if redirect_failure_count:
        raise RuntimeError(
            "hint metadata redirect-item envelope leaves "
            f"{redirect_failure_count} pairs unrenderable; "
            f"examples={redirect_failures}"
        )

    bumper_failures: list[str] = []
    bumper_identities: dict[bytes, str] = {}
    for item in emitted_items:
        identity = next(
            (
                encoded
                for encoded in (
                    _hint_encode_game_identity(
                        f"Cape prize is {item.qualified_name}"
                    ),
                    _hint_encode_game_identity(
                        f"Prize is {item.qualified_name}"
                    ),
                    _hint_encode_game_identity(item.qualified_name),
                )
                if encoded is not None
            ),
            None,
        )
        if identity is None:
            bumper_failures.append(item.registry_name)
            continue
        prior = bumper_identities.get(identity)
        if prior is not None:
            raise RuntimeError(
                "hint metadata final encoded Bumper identity collision: "
                f"{prior!r} and {item.registry_name!r}"
            )
        bumper_identities[identity] = item.registry_name
    if bumper_failures:
        raise RuntimeError(
            "hint metadata Bumper item envelope cannot render "
            f"{bumper_failures}"
        )

    explicit_region_aliases = _hint_mapping(
        doc.get("region_aliases"), "region_aliases"
    )
    if any(
        not isinstance(key, str)
        or not key
        or not isinstance(value, str)
        or not value
        for key, value in explicit_region_aliases.items()
    ):
        raise RuntimeError(
            "hint metadata region_aliases must map non-empty strings to strings"
        )
    emitted_regions: list[HintRegionMetadata] = []
    if regions is not None or region_ids is not None:
        if regions is None or region_ids is None:
            raise RuntimeError(
                "hint metadata region validation requires regions and region_ids"
            )
        unknown_region_aliases = set(explicit_region_aliases) - set(regions)
        exact_assetless_contract = (
            len(locations) == registry_contract["base_location_count"]
            and len(regions) == registry_contract["base_region_count"]
        )
        if unknown_region_aliases and not exact_assetless_contract:
            raise RuntimeError(
                "hint metadata region_aliases references unknown regions "
                f"{sorted(unknown_region_aliases)}"
            )
        compact_region_owner: dict[str, HintRegionMetadata] = {}
        qualified_region_owner: dict[str, HintRegionMetadata] = {}
        region_game_identities: set[bytes] = set()
        for region_id, registry_name in enumerate(region_ids):
            region = regions.get(registry_name)
            if region is None:
                raise RuntimeError(
                    f"hint metadata region id {registry_name!r} is not loaded"
                )
            qualified_name = region.name or _hint_split_identifier(
                registry_name
            )
            if (
                not qualified_name
                or len(qualified_name.encode("ascii", errors="strict")) >= 96
                or qualified_name != " ".join(qualified_name.split())
            ):
                raise RuntimeError(
                    f"hint metadata qualified region name for "
                    f"{registry_name!r} is invalid"
                )
            compact_name = _hint_compact_region_name(
                registry_name, qualified_name, explicit_region_aliases
            )
            if (
                not compact_name
                or len(compact_name.encode("ascii", errors="strict")) >= 64
                or compact_name != " ".join(compact_name.split())
            ):
                raise RuntimeError(
                    f"hint metadata compact region alias for "
                    f"{registry_name!r} is invalid"
                )
            row = HintRegionMetadata(
                region_id, registry_name, qualified_name, compact_name
            )
            prior = compact_region_owner.get(compact_name)
            if prior is not None:
                raise RuntimeError(
                    "hint metadata compact-region collision: "
                    f"{prior.registry_name!r} and {registry_name!r} share "
                    f"{compact_name!r}; add region_aliases entries"
                )
            prior = qualified_region_owner.get(qualified_name)
            if prior is not None:
                raise RuntimeError(
                    "hint metadata qualified-region collision: "
                    f"{prior.registry_name!r} and {registry_name!r} share "
                    f"{qualified_name!r}"
                )
            compact_region_owner[compact_name] = row
            qualified_region_owner[qualified_name] = row
            emitted_regions.append(row)
            for count in range(
                2, int(registry_contract["location_count"]) + 1
            ):
                full = f"{qualified_name} contains {count} useful items"
                game = f"{compact_name} has {count} useful"
                game_identity = _hint_encode_game_identity(game)
                if len(full.encode("ascii")) >= 160 or game_identity is None:
                    raise RuntimeError(
                        "hint metadata region-value envelope cannot render "
                        f"{registry_name!r} count={count}: {game!r}"
                    )
                if len(game.encode("ascii")) >= 96:
                    raise RuntimeError(
                        "hint metadata region-value game text exceeds its "
                        f"runtime buffer for {registry_name!r} count={count}"
                    )
                identity_digest = hashlib.blake2b(
                    game_identity, digest_size=16
                ).digest()
                if identity_digest in region_game_identities:
                    raise RuntimeError(
                        "hint metadata final encoded-glyph region-value "
                        f"identity collides at {registry_name!r} count={count}"
                    )
                region_game_identities.add(identity_digest)

    high_friction_names = _hint_string_list(
        doc.get("high_friction_locations"), "high_friction_locations"
    )
    unknown_high_friction = set(high_friction_names) - location_names
    if unknown_high_friction:
        raise RuntimeError(
            "hint metadata high_friction_locations references unknown locations "
            f"{sorted(unknown_high_friction)}"
        )
    high_friction_ids = tuple(
        sorted(locations[name].id for name in high_friction_names)
    )

    contract_location_values = sorted(
        (contract_locations or locations).values(), key=lambda loc: loc.id
    )
    contract_region_values = contract_regions or {}
    contract_region_order = contract_region_ids or sorted(
        contract_region_values
    )
    contract_world_state_rows = sorted(
        (
            str(location_key),
            int(world_state),
            override.id,
            override.name,
            override.region,
            override.type,
        )
        for location_key, world_states in (
            contract_world_state_overrides or {}
        ).items()
        for world_state, override in world_states.items()
    )
    algorithm_fingerprint_input = {
        "item_classification": doc["item_classification"],
        "diversity_groups": doc["diversity_groups"],
        "high_friction_locations": doc["high_friction_locations"],
        # Dungeon kind recognition feeds major classification. Display labels
        # belong exclusively to the text axis.
        "dungeon_kinds": sorted(doc["item_names"]["dungeon_kinds"]),
        "items": [
            (item.id, item.name, item.category)
            for item in sorted_items
        ],
        "locations": [
            (
                location.id,
                location.name,
                location.region,
                location.type,
            )
            for location in contract_location_values
        ],
        "region_id_order": list(contract_region_order),
        "world_state_region_overrides": contract_world_state_rows,
        "region_contract": [
            (
                region_id,
                contract_region_values[region_id].parent,
                contract_region_values[region_id].world_state_filter,
            )
            for region_id in contract_region_order
            if region_id in contract_region_values
        ],
        # Hash the whole approved policy object: base-only acceptance counts,
        # type counts, provenance, and both registry axes are all compatibility
        # inputs even when one field is redundant for this axis.
        "merged_registry_contract": registry_contract,
        "classification_algorithms": {
            "classification": inspect.getsource(_hint_classify_item),
            "dungeon_kind_recognition": inspect.getsource(
                _hint_dungeon_identity
            ),
        },
    }
    text_fingerprint_input = {
        "item_names": doc["item_names"],
        "dungeon_aliases": doc["dungeon_aliases"],
        "compact_dungeon_aliases": doc["compact_dungeon_aliases"],
        "location_base_aliases": doc["location_base_aliases"],
        "location_aliases": doc["location_aliases"],
        "region_aliases": doc["region_aliases"],
        "redirect_item_location_items": doc["redirect_item_location_items"],
        "items": [
            (item.id, item.name, item.category, item.display_name)
            for item in sorted_items
        ],
        "locations": [
            (location.id, location.name) for location in contract_location_values
        ],
        "regions": [
            (
                region_id,
                contract_region_values[region_id].name,
            )
            for region_id in contract_region_order
            if region_id in contract_region_values
        ],
        "merged_registry_contract": registry_contract,
        "naming_algorithms": {
            "item": inspect.getsource(_hint_item_names),
            "dungeon_identity": inspect.getsource(_hint_dungeon_identity),
            "identifier": inspect.getsource(_hint_split_identifier),
            "location": inspect.getsource(_hint_compact_location_name),
            "region": inspect.getsource(_hint_compact_region_name),
            "encoder": inspect.getsource(_hint_encode_game_identity),
            "fit": inspect.getsource(_hint_text_fits_game),
        },
    }
    algorithm_fingerprint = hashlib.sha256(
        json.dumps(
            algorithm_fingerprint_input,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("ascii")
    ).hexdigest()
    text_fingerprint = hashlib.sha256(
        json.dumps(
            text_fingerprint_input,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
        ).encode("ascii")
    ).hexdigest()
    return HintMetadata(
        format_version=1,
        algorithm_version=algorithm_version,
        text_schema_version=text_schema_version,
        algorithm_fingerprint=algorithm_fingerprint,
        text_fingerprint=text_fingerprint,
        items=tuple(emitted_items),
        locations=tuple(emitted_locations),
        regions=tuple(emitted_regions),
        high_friction_location_ids=high_friction_ids,
    )


def _hint_runtime_version(path: Path, macro: str) -> int:
    source = path.read_text(encoding="utf-8")
    match = re.search(
        rf"^\s*#define\s+{re.escape(macro)}\s+(\d+)u?\s*$",
        source,
        flags=re.MULTILINE,
    )
    if match is None:
        raise RuntimeError(
            f"cannot read {macro} from {path}"
        )
    return int(match.group(1))


def hint_runtime_algorithm_version(path: Path) -> int:
    return _hint_runtime_version(path, "kRandoHintPlanAlgorithmVersion")


def hint_runtime_text_schema_version(path: Path) -> int:
    return _hint_runtime_version(path, "kRandoHintTextSchemaVersion")


def validate_hint_metadata_lock(
    lock_path: Path,
    metadata: HintMetadata,
    runtime_algorithm_version: int,
    runtime_text_schema_version: int,
    *,
    update: bool = False,
) -> None:
    """Bind semantic selection and rendered text to separate compat axes."""
    if metadata.algorithm_version != runtime_algorithm_version:
        raise RuntimeError(
            "hint metadata algorithm-version drift: "
            f"hint_metadata.yaml={metadata.algorithm_version}, "
            f"kRandoHintPlanAlgorithmVersion={runtime_algorithm_version}"
        )
    if metadata.text_schema_version != runtime_text_schema_version:
        raise RuntimeError(
            "hint metadata text-schema drift: "
            f"hint_metadata.yaml={metadata.text_schema_version}, "
            f"kRandoHintTextSchemaVersion={runtime_text_schema_version}"
        )
    expected = {
        "format_version": 1,
        "algorithm_version": metadata.algorithm_version,
        "algorithm_fingerprint": metadata.algorithm_fingerprint,
        "text_schema_version": metadata.text_schema_version,
        "text_fingerprint": metadata.text_fingerprint,
    }
    current = None
    if lock_path.exists():
        current = json.loads(lock_path.read_text(encoding="utf-8"))
        if not isinstance(current, dict) or set(current) != set(expected):
            raise RuntimeError(
                f"{lock_path} has an invalid schema; expected exactly "
                f"{sorted(expected)}"
            )

    if update:
        algorithm_changed_without_bump = (
            current is not None
            and current.get("algorithm_fingerprint")
            != metadata.algorithm_fingerprint
            and current.get("algorithm_version")
            == metadata.algorithm_version
        )
        text_changed_without_bump = (
            current is not None
            and current.get("text_fingerprint") != metadata.text_fingerprint
            and current.get("text_schema_version")
            == metadata.text_schema_version
        )
        if algorithm_changed_without_bump or text_changed_without_bump:
            axes = []
            if algorithm_changed_without_bump:
                axes.append("algorithm_version")
            if text_changed_without_bump:
                axes.append("text_schema_version")
            raise RuntimeError(
                "hint metadata compatibility fingerprint changed without "
                f"bumping {', '.join(axes)} in hint_metadata.yaml and the "
                "matching rando_hints.h constant"
            )
        atomic_write_text(
            lock_path,
            json.dumps(expected, indent=2, sort_keys=True) + "\n",
        )
        return

    if current is None:
        raise RuntimeError(
            f"{lock_path} is missing; create it intentionally with "
            "--update-hint-metadata-lock"
        )
    if current != expected:
        raise RuntimeError(
            "hint metadata compatibility fingerprint drift: approved "
            f"algorithm={current.get('algorithm_version')}/"
            f"{current.get('algorithm_fingerprint')} text="
            f"{current.get('text_schema_version')}/"
            f"{current.get('text_fingerprint')}; current algorithm="
            f"{metadata.algorithm_version}/{metadata.algorithm_fingerprint} "
            f"text={metadata.text_schema_version}/{metadata.text_fingerprint}. "
            "Bump the affected runtime + YAML compatibility axis, then run "
            "rando_logic_gen.py --update-hint-metadata-lock."
        )


def _pot_key_info(room, pot_id, pot_room_wrap, pot_key_wrap):
    """Return (item, full, dungeon) pot-key requirements for one pot.

    A KEY pot uses its EXACT per-pot dungeon depth (and its own wild `full` when
    the room has no entry); a loot/empty pot uses the room's wild `full` and
    room-max dungeon.
    """
    room_w = pot_room_wrap.get(room)
    key_w = pot_key_wrap.get(pot_id)
    item = (room_w[0] if room_w else None) or (key_w[1] if key_w else None)
    if item is None:
        return None, None, None
    # WILD: room-max `full` for every pot; the key pot's own `full` only backfills
    # when the room has no wild entry (keeps clean rooms byte-identical under wild).
    full = room_w[1] if room_w else None
    if full is None and key_w is not None:
        full = key_w[2]
    # DUNGEON: key pot -> exact per-pot depth; loot/empty -> room-max.
    dungeon = key_w[3] if key_w is not None else (room_w[2] if room_w else None)
    return item, full, dungeon


def _suppress_base_key_terms_under_dungeon(expr, item, dungeon):
    """Let generated DUNGEON pot-key depth replace inherited same-key gates.

    Some pot rows inherit a coarse room predicate such as HAS_AMOUNT(SmallKey_GT,
    4), but the pot-key-depth table knows the exact in-context requirement for
    this pot/room. Wrap same-dungeon small-key predicates so they are preserved
    outside DUNGEON pot-key mode and suppressed inside it; the generated
    POT_KEYS_DUNGEON tail then supplies the exact requirement, including zero.
    """
    if item is None or dungeon is None:
        return expr

    item_pat = re.escape(item)

    def repl(match):
        return f"(POT_KEYS_DUNGEON() OR {match.group(0)})"

    expr = re.sub(rf"HAS_AMOUNT\(\s*{item_pat}\s*,\s*\d+\s*\)", repl, expr)
    expr = re.sub(rf"HAS_ITEM\(\s*{item_pat}\s*\)", repl, expr)
    return expr


def _strip_door_vanilla_key_terms(expr: str, dungeon: int | None) -> str:
    """Door bridge predicates must not inherit vanilla same-dungeon key locks.

    The bridge asks the door oracle whether the door-shuffled region containing
    the check is reachable. Any vanilla SmallKey/BigKey terms from the room's
    original predicate would double-count or contradict that oracle, so strip
    only terms for this same dungeon before compiling the bridge predicate.
    """
    if dungeon is None or dungeon < 0 or dungeon >= len(DUNGEON_SUFFIXES):
        return expr
    suffix = DUNGEON_SUFFIXES[dungeon]
    for item in (f"SmallKey_{suffix}", f"BigKey_{suffix}"):
        item_pat = re.escape(item)
        expr = re.sub(rf"HAS_AMOUNT\(\s*{item_pat}\s*,\s*\d+\s*\)", "TRUE()", expr)
        expr = re.sub(rf"HAS_ITEM\(\s*{item_pat}\s*\)", "TRUE()", expr)
    return expr


def _suppress_base_key_terms_under_enemy_dungeon(expr, item, dungeon):
    """Let generated enemy-drop DUNGEON depth replace inherited same-key gates."""
    if item is None or dungeon is None:
        return expr

    item_pat = re.escape(item)

    def repl(match):
        return f"(ENEMY_DROP_KEYS_DUNGEON() OR {match.group(0)})"

    expr = re.sub(rf"HAS_AMOUNT\(\s*{item_pat}\s*,\s*\d+\s*\)", repl, expr)
    expr = re.sub(rf"HAS_ITEM\(\s*{item_pat}\s*\)", repl, expr)
    return expr


def _enemy_drop_key_terms_for(item, wild, dungeon, combined_wild=None):
    if item is None:
        return ""
    t = ""
    if wild is not None and wild > 0:
        t += f" AND (NOT ENEMY_DROP_KEYS_WILD() OR HAS_AMOUNT({item}, {wild}))"
    if combined_wild is not None and combined_wild > (wild or 0):
        t += (
            f" AND (NOT (ENEMY_DROP_KEYS_WILD() AND POT_KEYS_WILD()) "
            f"OR HAS_AMOUNT({item}, {combined_wild}))"
        )
    if dungeon is not None and dungeon > 0:
        t += f" AND (NOT ENEMY_DROP_KEYS_DUNGEON() OR HAS_AMOUNT({item}, {dungeon}))"
    return t


def _enemy_drop_big_key_terms_for(item):
    if item is None:
        return ""
    return (
        f" AND (NOT ENEMY_DROP_KEYS_WILD() OR HAS_ITEM({item}))"
        f" AND (NOT ENEMY_DROP_KEYS_DUNGEON() OR HAS_ITEM({item}))"
    )


def _pot_key_terms_for(item, full, dungeon):
    """Build the trailing pot-key gate for one generated key-depth row."""
    t = ""
    if full is not None and full > 0:
        t += f" AND (NOT POT_KEYS_WILD() OR HAS_AMOUNT({item}, {full}))"
    if dungeon is not None and dungeon > 0:
        t += f" AND (NOT POT_KEYS_DUNGEON() OR HAS_AMOUNT({item}, {dungeon}))"
    return t


def _pot_min_tier(kind: str | None, vanilla_item: str) -> int:
    if kind == "empty" or vanilla_item == "Nothing":
        return 3  # All
    if vanilla_item.startswith("SmallKey_"):
        return 1  # Keys
    return 2      # Contents


def _fnv32_u8(h: int, b: int) -> int:
    h ^= b & 0xFF
    return (h * 0x01000193) & 0xFFFFFFFF


def _fnv32_u16(h: int, v: int) -> int:
    h = _fnv32_u8(h, v)
    return _fnv32_u8(h, v >> 8)


def _fnv32_u32(h: int, v: int) -> int:
    h = _fnv32_u16(h, v)
    return _fnv32_u16(h, v >> 16)


def _door_pot_bridge_digest(rows: list[dict]) -> int:
    h = 0x811C9DC5
    for r in rows:
        h = _fnv32_u16(h, r["loc_id"])
        h = _fnv32_u8(h, r["dungeon"])
        h = _fnv32_u8(h, r["min_tier"])
        h = _fnv32_u8(h, r["flags"])
        h = _fnv32_u16(h, r["drop_index"])
        h = _fnv32_u8(h, len(r["regions"]))
        for region in r["regions"]:
            h = _fnv32_u16(h, region)
        h = _fnv32_u32(h, len(r["pred"]))
        for b in r["pred"]:
            h = _fnv32_u8(h, b)
    return h


def _pot_registry_digest(rows: list[tuple[int, int, int]]) -> int:
    if not rows:
        return 0
    h = 0x811C9DC5
    for room, pos4, locid in sorted(rows, key=lambda r: (r[0], r[1], r[2])):
        h = _fnv32_u16(h, room)
        h = _fnv32_u16(h, pos4)
        h = _fnv32_u16(h, locid)
    return h


def _terrain_registry_digest(rows: list[tuple[int, int, int, int]]) -> int:
    # add-rando-grass-rock-shuffle: FNV over the sorted
    # (screen, pos, loc_id, ws_mask) rows — the activation-guard value
    # (kRandoTerrainRegistryDigest), mirroring _pot_registry_digest. ws_mask
    # (world_state_filter) is folded in so a filter-only registry change is
    # drift-detected even without a kGeneratorVersion bump (fresh-eyes review).
    if not rows:
        return 0
    h = 0x811C9DC5
    for screen, pos, locid, ws_mask in sorted(rows, key=lambda r: (r[0], r[1], r[2])):
        h = _fnv32_u16(h, screen)
        h = _fnv32_u16(h, pos)
        h = _fnv32_u16(h, locid)
        h = _fnv32_u8(h, ws_mask)
    return h


def _bonk_registry_digest(rows) -> int:
    # add-rando-bonk-sanity: FNV over the sorted (area, block, loc_id) rows —
    # the activation-guard value (kRandoBonkRegistryDigest), mirroring
    # _terrain_registry_digest. Bonk rows are world-state-universal (the OW
    # sprite tables are selected by sram_progress_indicator only), so there
    # is no ws_mask component.
    #
    # STRUCTURAL-ONLY caveat: predicate content is invisible to this digest,
    # and normal builds (Makefile/vcxproj) only CONSUME bonk.gen.yaml — they
    # never regenerate it. A stale-predicate registry therefore compiles
    # silently under a matching sidecar/snapshot guard; run_rando_local_checks
    # regenerates it before codegen (round-2 fix) and a local corpus run
    # catches the drift via digest mismatch, but a build alone does not.
    if not rows:
        return 0
    h = 0x811C9DC5
    for area, block, locid in sorted(rows):
        h = _fnv32_u16(h, area)
        h = _fnv32_u16(h, block)
        h = _fnv32_u16(h, locid)
    return h


def _enemy_check_registry_digest(dungeon_rows, overworld_rows, boss_rows,
                                 scripted_rows) -> int:
    # FNV over the four sorted enemy-check lookup domains, in emit order, each
    # prefixed with its row count — the sidecar/snapshot activation-guard value
    # (kRandoEnemyCheckRegistryDigest), mirroring _terrain_registry_digest.
    # Every emitted field is folded (incl. all_tier_only) so a
    # same-kGeneratorVersion registry change — renumbered loc_ids, added or
    # dropped rows, tier flips — is drift-detected.
    if not (dungeon_rows or overworld_rows or boss_rows or scripted_rows):
        return 0
    h = 0x811C9DC5
    h = _fnv32_u32(h, len(dungeon_rows))
    for r in dungeon_rows:
        h = _fnv32_u16(h, int(r["room"]))
        h = _fnv32_u8(h, int(r["source_slot"]))
        h = _fnv32_u16(h, int(r["loc_id"]))
        h = _fnv32_u8(h, 1 if r.get("all_tier_only") else 0)
    h = _fnv32_u32(h, len(overworld_rows))
    for r in overworld_rows:
        h = _fnv32_u8(h, int(r["area"]))
        h = _fnv32_u8(h, int(r["stage"]))
        h = _fnv32_u8(h, int(r["source_slot"]))
        h = _fnv32_u16(h, int(r["block"]))
        h = _fnv32_u16(h, int(r["loc_id"]))
    h = _fnv32_u32(h, len(boss_rows))
    for r in boss_rows:
        h = _fnv32_u8(h, int(r["game_dungeon"]))
        h = _fnv32_u16(h, int(r["room"]))
        h = _fnv32_u8(h, 2 if r.get("boss_kind") == "gt_miniboss" else 1)
        h = _fnv32_u16(h, int(r["loc_id"]))
    h = _fnv32_u32(h, len(scripted_rows))
    for r in scripted_rows:
        h = _fnv32_u16(h, int(r["room"]))
        h = _fnv32_u8(h, int(r["parent_source_slot"]))
        h = _fnv32_u8(h, int(r["overlord_type"]))
        h = _fnv32_u8(h, int(r["child_index"]))
        h = _fnv32_u8(h, int(r["child_type"]))
        h = _fnv32_u16(h, int(r["loc_id"]))
    return h


def _door_enemy_drop_bridge_digest(rows: list[dict]) -> int:
    h = 0x811C9DC5
    for r in rows:
        h = _fnv32_u16(h, r["loc_id"])
        h = _fnv32_u8(h, r["dungeon"])
        h = _fnv32_u16(h, r["region"])
        h = _fnv32_u16(h, r["drop_index"])
        h = _fnv32_u32(h, len(r["pred"]))
        for b in r["pred"]:
            h = _fnv32_u8(h, b)
    return h


def _door_enemy_check_bridge_digest(rows: list[dict]) -> int:
    h = 0x811C9DC5
    for r in rows:
        h = _fnv32_u16(h, r["loc_id"])
        h = _fnv32_u8(h, r["dungeon"])
        h = _fnv32_u8(h, r["min_tier"])
        h = _fnv32_u8(h, len(r["regions"]))
        for region in r["regions"]:
            h = _fnv32_u16(h, region)
        h = _fnv32_u32(h, len(r["pred"]))
        for b in r["pred"]:
            h = _fnv32_u8(h, b)
    return h


def load_pots(path: Path, logic_regions: dict[str, RegionDef] | None = None):
    """Load assets/rando/pots.gen.yaml (local registry from gen_pot_tables.py).

    Returns (name -> LocationDef, [(room, pos4, loc_id)], bridge_source_rows).
    Each pot LocationDef carries id/name/region/type=Pot/vanilla_item AND
    can_reach (D8 inheritance), so it feeds BOTH the location registry
    (location_ids.h / kRandoLocations) and the logic binding (region_id +
    can_reach) - it is merged into both `locations` and `logic_loc_preds` in
    main(). pots.gen.yaml is a gitignored local artifact, so public/assetless
    builds emit no pot locations; generation fails closed if a user enables a
    pot tier without rebuilding from local pot codegen."""
    if not path.exists():
        print(f"WARNING: {path} not found - emitting NO pot locations. "
              f"Run assets/scripts/run_rando_local_checks.py with ROM assets "
              f"to enable pot shuffle in this build.", file=sys.stderr)
        return {}, [], []
    depth_tbl = RANDO_ASSETS / "pot_key_depth.gen.yaml"
    if not depth_tbl.exists():
        raise RuntimeError(
            f"{path} exists but {depth_tbl} is missing. This partial local pot "
            f"artifact set would emit pot locations without POT_KEYS_WILD/"
            f"POT_KEYS_DUNGEON key-depth wraps or door+pot bridge rows. Run "
            f"assets/scripts/run_rando_local_checks.py --prepare-only with a "
            f"fresh binary, or remove {path} for an assetless build."
        )
    doc = load_yaml(path)
    # add-rando-pot-sanity task #25: small-key requirements pot_shuffle adds once a
    # dungeon's pot keys are first-class checks. A pot behind key doors must require
    # those keys or a progression item placed there strands. pots-off keeps both
    # POT_KEYS_WILD and POT_KEYS_DUNGEON false, so the wrap collapses to the vanilla
    # predicate (byte-identical). pot_rooms gives every pot in the room a WILD `full`
    # term and loot/empty pots a DUNGEON room-max term; a KEY pot uses its EXACT
    # per-pot dungeon depth (pot_keys), plus its own `full` when the room has no wild
    # entry (the floor-bit Waterway orphan). See gen_pot_key_depth.py.
    pot_room_wrap = {}     # room (int) -> (item, full|None, dungeon_depth|None)
    pot_key_wrap = {}      # pot id (int) -> (dungeon, item, full|None, dungeon_depth, door_region, drop_index)
    door_room_wrap = {}    # room (int) -> (dungeon, [door_region ids])
    dt = load_yaml(depth_tbl)
    if int(dt.get("format_version", 0) or 0) < 3:
        sys.exit(f"{depth_tbl}: stale format_version; regenerate with "
                 "assets/scripts/gen_pot_key_depth.py so door+pot bridge rows "
                 "and drop-key indices are available.")
    door_pot_room_rows = dt.get("door_pot_rooms")
    if not door_pot_room_rows:
        sys.exit(f"{depth_tbl}: missing door_pot_rooms bridge rows; regenerate "
                 "with assets/scripts/gen_pot_key_depth.py after "
                 "--dump-key-depth.")
    for r in dt.get("pot_rooms", []) or []:
        pot_room_wrap[int(r["room"])] = (
            r["item"],
            int(r["full"]) if "full" in r else None,
            int(r["dungeon"]) if "dungeon" in r else None,
        )
    for r in dt.get("pot_keys", []) or []:
        if "door_region" in r and "depth" not in r:
            sys.exit(f"{depth_tbl}: stale pot_keys row id={r.get('id')} uses "
                     "`dungeon` as a depth; regenerate with "
                     "assets/scripts/gen_pot_key_depth.py")
        pot_key_wrap[int(r["id"])] = (
            int(r["dungeon"]),
            r["item"],
            int(r["full"]) if "full" in r else None,
            int(r["depth"]) if "depth" in r else int(r["dungeon"]),
            int(r["door_region"]),
            int(r["drop_index"]),
        )
    for r in door_pot_room_rows or []:
        door_room_wrap[int(r["room"])] = (
            int(r["dungeon"]),
            [int(x) for x in r.get("regions", []) or []],
        )
    out, rows, bridge_rows = {}, [], []
    for p in doc.get("pots", []) or []:
        # add-rando-pot-sanity: empty pots carry the ITEM_Nothing filler as their
        # vanilla item. The generated pots.gen.yaml records empties as
        # `kind: empty` + `vanilla_item: null`; map that to "Nothing" here so the
        # generated kRandoLocations row has vanilla_item_id == ITEM_Nothing (148).
        # That id is the unambiguous empty-pot signal the placer keys on
        # (`pot_active`, the §3b ITEM_Nothing pre-pass) — id 0 is ProgressiveSword,
        # so a null→0 mapping would be indistinguishable from a real item.
        vanilla_item = p.get("vanilla_item") or ""
        if (p.get("kind") == "empty") or not vanilla_item:
            vanilla_item = "Nothing"
        base_can_reach = p.get("can_reach") or "TRUE()"
        can_reach = base_can_reach
        item, full, dungeon = _pot_key_info(
            int(p["room"]), int(p["id"]), pot_room_wrap, pot_key_wrap)
        can_reach = _suppress_base_key_terms_under_dungeon(can_reach, item, dungeon)
        terms = _pot_key_terms_for(item, full, dungeon) if item is not None else ""
        if terms:
            can_reach = f"({can_reach}){terms}"
        loc = LocationDef(
            id=p["id"], name=p["name"], region=p.get("region", ""), type="Pot",
            vanilla_item=vanilla_item,
            can_reach=can_reach,
            can_place="TRUE()", always_allow="FALSE()", source="gen_pot_tables.py",
        )
        out[loc.name] = loc
        rows.append((int(p["room"]), int(p["pos4"]), int(p["id"])))
        min_tier = _pot_min_tier(p.get("kind"), vanilla_item)
        flags = 0
        if vanilla_item != "Nothing":
            flags |= 0x01  # kDoorPot_KeySource
        if vanilla_item.startswith("SmallKey_"):
            flags |= 0x02  # kDoorPot_KeyPot
        if vanilla_item == "Nothing":
            flags |= 0x04  # kDoorPot_Empty

        dungeon = 0xFF
        regions = []
        drop_index = 0xFFFF
        if flags & 0x02:
            kw = pot_key_wrap.get(int(p["id"]))
            if kw is None and depth_tbl.exists():
                sys.exit(f"pot bridge: key pot id={p['id']} has no pot_keys row in {depth_tbl}")
            if kw is not None:
                dungeon, _item, _full, _depth, door_region, drop_index = kw
                regions = [door_region]
        else:
            rw = door_room_wrap.get(int(p["room"]))
            if rw is not None:
                dungeon, regions = rw
        if regions and dungeon != 0xFF:
            bridge_can_reach = _strip_door_vanilla_key_terms(base_can_reach, dungeon)
            bridge_rows.append({
                "loc_id": int(p["id"]),
                "name": p["name"],
                "dungeon": dungeon,
                "regions": sorted(set(regions)),
                "base_can_reach": bridge_can_reach,
                "min_tier": min_tier,
                "flags": flags,
                "drop_index": drop_index,
            })
    return out, rows, bridge_rows


def _load_pot_key_counts_by_item() -> dict[str, int]:
    """Return item -> active key-pot count from the local pot key-depth table."""
    table_path = RANDO_ASSETS / "pot_key_depth.gen.yaml"
    if not table_path.exists():
        return {}
    doc = load_yaml(table_path)
    counts: dict[str, int] = {}
    for row in doc.get("pot_keys", []) or []:
        item = row.get("item")
        if item:
            counts[item] = counts.get(item, 0) + 1
    return counts


def _enemy_source_counts(doc: dict) -> dict[str, dict[str, int]]:
    out: dict[str, dict[str, int]] = {}
    for row in doc.get("small_key_source_counts", []) or []:
        item = row["item"]
        out[item] = {
            "chest": int(row.get("chest_count", 0) or 0),
            "drop": int(row.get("drop_count", 0) or 0),
            "enemy": int(row.get("enemy_drop_count", 0) or 0),
        }
    if out:
        return out
    # Back-compat for stale local artifacts during development; strict format
    # checks below normally force regeneration before this path matters.
    for row in doc.get("enemy_drops", []) or []:
        if row.get("drop") != "small_key":
            continue
        item = row.get("vanilla_item")
        if item:
            cur = out.setdefault(item, {"chest": 0, "drop": 0, "enemy": 0})
            cur["enemy"] += 1
    return out


# Vanilla placed/chest small-key counts per dungeon (structural fact; mirrors
# the centralized Rando_BaseSmallKeyCount metadata in src/rando/dungeon_ids.h /
# ALTTPR config `small_keys.*`). _enemy_depth_terms needs a WILD cap for EVERY key dungeon,
# but enemy_drops.gen.yaml's small_key_source_counts only carries rows for the
# dungeons that HAVE forced enemy key drops — Swamp/Thieves'/Desert/Hera/PoD
# have none, and the old "no row -> leave wild uncapped" fallback emitted their
# raw pot-inclusive door depth (e.g. HAS_AMOUNT(SmallKey_SwampPalace, 6)) as a
# hard WILD requirement. Whenever pot keys are NOT itemized (pots off, or pots
# forced off by cave entrance shuffle) the wild pool only holds the chest keys
# (SP/TT/DP: 1 each), so those gates were permanently unsatisfiable — the
# constant 63-location enemy-check strand under entrance shuffle × wild keys ×
# enemy_drop_checks. The pot-inclusive bound belongs ONLY in the separate
# combined term guarded by POT_KEYS_WILD(); with pots off the pot keys drop
# vanilla-free at runtime, so they can never be an external requirement.
VANILLA_CHEST_KEY_COUNTS: dict[str, int] = {
    "SmallKey_HyruleCastleEscape": 1,
    "SmallKey_EasternPalace": 0,
    "SmallKey_DesertPalace": 1,
    "SmallKey_TowerOfHera": 1,
    "SmallKey_HyruleCastleTower": 2,
    "SmallKey_PalaceOfDarkness": 6,
    "SmallKey_SwampPalace": 1,
    "SmallKey_SkullWoods": 3,
    "SmallKey_ThievesTown": 1,
    "SmallKey_IcePalace": 2,
    "SmallKey_MiseryMire": 3,
    "SmallKey_TurtleRock": 4,
    "SmallKey_GanonsTower": 4,
}


def _enemy_depth_terms(row: dict, item: str, source_counts: dict[str, dict[str, int]],
                       pot_key_counts: dict[str, int]) -> tuple[int | None, int | None, int | None]:
    raw_wild = int(row["key_depth"]) if "key_depth" in row else 0
    raw_dungeon = int(row["key_mindepth"]) if "key_mindepth" in row else 0
    counts = source_counts.get(item)
    if counts is not None:
        chest = int(counts.get("chest", 0))
        # Drift guard: the generated chest count is the same vanilla fact as the
        # fallback table; a disagreement means a stale/renamed artifact.
        if item in VANILLA_CHEST_KEY_COUNTS and chest != VANILLA_CHEST_KEY_COUNTS[item]:
            raise RuntimeError(
                f"small_key_source_counts chest_count for {item} is {chest}, "
                f"expected the vanilla {VANILLA_CHEST_KEY_COUNTS[item]} — "
                f"regenerate enemy_drops.gen.yaml (gen_enemy_drop_tables.py)")
        enemy_cap = chest + int(counts.get("enemy", 0))
    else:
        # No source row = a dungeon with no forced enemy key drops. Its only
        # wild-poolable keys are the vanilla chest keys; cap there instead of
        # failing open to the raw (pot-inclusive) depth. A cap of 0 emits no
        # WILD term at all (every key door en route is fed by free drops).
        enemy_cap = VANILLA_CHEST_KEY_COUNTS.get(item, 0)
    wild = min(raw_wild, enemy_cap)
    combined_cap = enemy_cap + int(pot_key_counts.get(item, 0))
    combined_wild = min(raw_wild, combined_cap) if combined_cap > 0 else wild
    if combined_wild <= wild:
        combined_wild = None
    return wild, raw_dungeon, combined_wild


def _apply_enemy_drop_key_terms(loc_preds, world_state_overrides, key_rows, hce_big_key_locations):
    """Wrap ordinary locations whose vanilla logic assumed forced enemy drops were free."""
    for row in key_rows or []:
        name, item = row["name"], row["item"]
        wild = row.get("wild")
        dungeon = row.get("dungeon")
        combined_wild = row.get("combined_wild")
        tail = _enemy_drop_key_terms_for(item, wild, dungeon, combined_wild)
        applied = False
        if name in loc_preds:
            if tail:
                loc_preds[name].can_reach = f"({loc_preds[name].can_reach}){tail}"
            applied = True
        for ld in world_state_overrides.get(name, {}).values():
            if tail:
                ld.can_reach = f"({ld.can_reach}){tail}"
            applied = True
        if not applied:
            print(f"WARNING: enemy key-depth location {name!r} matches no logic "
                  f"location — stale table or renamed location?")

    big_key_tail = _enemy_drop_big_key_terms_for("HyruleCastleBigKey")
    for name in hce_big_key_locations or []:
        applied = False
        if name in loc_preds:
            loc_preds[name].can_reach = f"({loc_preds[name].can_reach}){big_key_tail}"
            applied = True
        for ld in world_state_overrides.get(name, {}).values():
            ld.can_reach = f"({ld.can_reach}){big_key_tail}"
            applied = True
        if not applied:
            print(f"WARNING: HCE big-key location {name!r} matches no logic location")


def load_soul_rooms(path: Path):
    """Load assets/rando/soul_rooms.gen.yaml (add-enemy-souls task 1.4/4.4).

    Local/ROM-derived and optional, like the pot/enemy registries. When absent,
    no soul wraps are applied and logic_data.c emits kRandoSoulRoomsBaked=0 —
    souls_shuffle=all seeds then fail closed in BuildItemPool. Returns
    (baked, loc_id_wraps, region_wraps, name_wraps, room_wraps, pin_rooms).
    """
    empty = (False, {}, {}, {}, {}, [])
    if not path.exists():
        print(f"WARNING: {path} not found - kill-gated-room soul requirements "
              f"not baked. Run assets/scripts/gen_soul_room_tables.py with ROM "
              f"assets to enable souls_shuffle=all in this build.", file=sys.stderr)
        return empty
    doc = load_yaml(path)
    if int(doc.get("format_version", 0) or 0) != 1:
        raise RuntimeError(f"{path}: unsupported format_version {doc.get('format_version')!r}")
    loc_id_wraps = {int(r["loc_id"]): list(r["souls"]) for r in doc.get("location_wraps", []) or []}
    region_wraps = {int(r["region"]): list(r["souls"]) for r in doc.get("region_wraps", []) or []}
    name_wraps = {str(r["name"]): list(r["souls"]) for r in doc.get("name_wraps", []) or []}
    room_wraps = {int(r["room"]): list(r["souls"]) for r in doc.get("room_wraps", []) or []}
    pin_rooms = sorted(int(r) for r in doc.get("pin_rooms", []) or [])
    if not loc_id_wraps or not pin_rooms:
        raise RuntimeError(f"{path}: empty location_wraps/pin_rooms — regenerate (corrupt table?)")
    for wraps in (loc_id_wraps, region_wraps, name_wraps, room_wraps):
        for souls in wraps.values():
            if not souls:
                raise RuntimeError(f"{path}: wrap row with empty soul list")
    return True, loc_id_wraps, region_wraps, name_wraps, room_wraps, pin_rooms


def _soul_terms(souls: list[str]) -> str:
    """` AND NeedsEnemySoul(Soul_X)...` tail — inert below the enemies tier
    (the macro is (NOT OP_SOULS_TIER_AT_LEAST(bosses_enemies)) OR HAS_ITEM)."""
    return "".join(f" AND NeedsEnemySoul(Soul_{s})" for s in souls)


def _npc_soul_terms(souls: list[str]) -> str:
    """` AND NeedsNpcSoul(Soul_Npc_X)...` tail — inert when npc_souls=off."""
    return "".join(f" AND NeedsNpcSoul(Soul_Npc_{s})" for s in souls)


def _apply_npc_soul_gates(npc_path: Path, logic_loc_preds,
                          world_state_overrides, logic_edges):
    """AND NPC-soul terms into the gated checks + the Kiki PoD entry edge.

    add-npc-souls. npc_souls.yaml is COMMITTED (generated by
    gen_npc_soul_tables.py) — absence is a hard error, not fail-open. Target
    misses are HARD errors (a silently-missed gate lets the placer route
    progression through a check whose person the runtime suppresses).
    Runs alongside the enemy-souls wraps, BEFORE world-state override
    compilation and the door-shuffle wrap (no gated check is door-controlled).
    Edge gates AND their terms into the BASE edge only; Inverted's additive
    TRUE() PoD edge deliberately bypasses the Kiki term (its runtime gate is
    open in Inverted — verified in task 1.4).
    """
    if not npc_path.exists():
        raise RuntimeError(
            f"{npc_path} missing — it is a COMMITTED generated file; run "
            f"assets/scripts/gen_npc_soul_tables.py")
    doc = load_yaml(npc_path)
    for gate in doc.get("gates", []):
        name = gate["location"]
        tail = _npc_soul_terms(gate["souls"])
        applied = False
        if name in logic_loc_preds:
            ld = logic_loc_preds[name]
            ld.can_reach = f"({ld.can_reach}){tail}"
            applied = True
        for ld in world_state_overrides.get(name, {}).values():
            ld.can_reach = f"({ld.can_reach}){tail}"
            applied = True
        if not applied:
            raise RuntimeError(
                f"npc_souls.yaml gate target {name!r} has no logic predicate "
                f"to wrap — NPC check would ship unguarded")
    for eg in doc.get("edge_gates", []):
        tail = _npc_soul_terms(eg["souls"])
        hits = 0
        for e in logic_edges or []:
            if e.from_ == eg["from"] and e.to == eg["to"]:
                e.predicate = f"({e.predicate}){tail}"
                hits += 1
        if hits == 0:
            raise RuntimeError(
                f"npc_souls.yaml edge gate {eg['from']} -> {eg['to']} matches "
                f"no logic edge — PoD entry would ship un-gated")


def _apply_soul_room_wraps(soul_rooms, locations, logic_loc_preds,
                           world_state_overrides,
                           enemy_drop_locs, enemy_drop_lookup_rows,
                           enemy_drops_path: Path,
                           enemy_check_locs, enemy_checks_path: Path,
                           pot_locs, pots_path: Path,
                           logic_edges=None):
    """AND the kill-gated rooms' resident-soul terms into every gated predicate.

    Soundness-critical: a silently-missed wrap lets the placer route
    progression through a room the runtime holds shut, so target misses are
    HARD errors, not warnings. Four wrap surfaces:
      - fork locations by id (chests/bosses/prizes from kDoorTblLocations +
        reviewed cave bindings),
      - curated event sites by logic-location name (Aga1),
      - generated enemy-drop/enemy-check locations by door-graph region,
      - generated pot locations by room.
    """
    (baked, loc_id_wraps, region_wraps, name_wraps, room_wraps, _pins) = soul_rooms
    if not baked:
        return
    id_to_name = {loc.id: loc.name for loc in locations.values()}

    def wrap_by_name(name: str, souls: list[str], what: str):
        tail = _soul_terms(souls)
        applied = False
        if name in logic_loc_preds:
            ld = logic_loc_preds[name]
            ld.can_reach = f"({ld.can_reach}){tail}"
            applied = True
        for ld in world_state_overrides.get(name, {}).values():
            ld.can_reach = f"({ld.can_reach}){tail}"
            applied = True
        if not applied:
            raise RuntimeError(
                f"soul_rooms.gen.yaml {what} wrap target {name!r} has no logic "
                f"predicate to wrap — kill-gated location would ship unguarded")

    for loc_id, souls in sorted(loc_id_wraps.items()):
        name = id_to_name.get(loc_id)
        if name is None:
            raise RuntimeError(
                f"soul_rooms.gen.yaml location wrap id={loc_id} matches no "
                f"registry location — stale table?")
        wrap_by_name(name, souls, f"location id={loc_id}")
    for name, souls in sorted(name_wraps.items()):
        wrap_by_name(name, souls, "event site")

    # Dungeon-chains factored boss-room APPROACH edges (fresh-eyes review):
    # under chains the successor dungeon's entry redirects through
    # <D>_BossRoom, so the approach edge must carry the same kill-room soul
    # requirements as the dungeon's Boss location or the placer certifies the
    # successor while the previous dungeon is soul-held. Off-chains the edge
    # only gates Boss/Prize, whose own predicates already carry these terms —
    # so the added terms are logically inert there.
    if logic_edges is not None:
        for e in logic_edges:
            if e.source != "dungeon-chains generated boss-room factor":
                continue
            boss_souls = set()
            for name, ld in logic_loc_preds.items():
                if ld.region != e.to:
                    continue
                loc = locations.get(name)
                if loc is not None and loc.id in loc_id_wraps:
                    boss_souls.update(loc_id_wraps[loc.id])
            if boss_souls:
                e.predicate = f"({e.predicate}){_soul_terms(sorted(boss_souls))}"

    # Generated enemy-drop locations, by door-graph region id.
    for row in enemy_drop_lookup_rows or []:
        souls = region_wraps.get(int(row["door_region"]))
        if not souls:
            continue
        name = id_to_name.get(int(row["loc_id"]))
        if name is None or name not in enemy_drop_locs:
            raise RuntimeError(f"enemy-drop soul wrap: loc {row['loc_id']} not loaded")
        ld = enemy_drop_locs[name]
        ld.can_reach = f"({ld.can_reach}){_soul_terms(souls)}"

    # Generated ordinary enemy checks, by door-region list (raw yaml carries it).
    region_wrap_hits = 0
    for row in enemy_drop_lookup_rows or []:
        if region_wraps.get(int(row["door_region"])):
            region_wrap_hits += 1
    if enemy_check_locs and enemy_checks_path.exists():
        doc = load_yaml(enemy_checks_path)
        for raw in doc.get("enemy_checks", []) or []:
            regions = [int(x) for x in raw.get("door_regions", []) or []]
            if not regions and "door_region" in raw:
                regions = [int(raw["door_region"])]
            souls = set()
            for reg in regions:
                souls.update(region_wraps.get(reg, []))
            if not souls:
                continue
            region_wrap_hits += 1
            name = raw["name"]
            if name not in enemy_check_locs:
                continue  # audit-only / excluded row
            ld = enemy_check_locs[name]
            ld.can_reach = f"({ld.can_reach}){_soul_terms(sorted(souls))}"
    # Drift guard (fresh-eyes review): the region-id space is shared between
    # soul_rooms.gen.yaml and the enemy-drop/check tables (both derive from the
    # door graph). A renumbering that broke the join would silently drop every
    # region wrap; zero hits with generated rows present is that signature.
    if region_wraps and (enemy_drop_lookup_rows or enemy_check_locs) and \
            region_wrap_hits == 0:
        print("WARNING: soul_rooms.gen.yaml region wraps matched ZERO generated "
              "enemy locations — door-graph region-id drift? Regenerate "
              "soul_rooms/enemy tables from the same assets.", file=sys.stderr)

    # Generated pot locations, by room.
    if pot_locs and pots_path.exists():
        doc = load_yaml(pots_path)
        for raw in doc.get("pots", []) or []:
            souls = room_wraps.get(int(raw["room"]))
            if not souls:
                continue
            name = raw["name"]
            if name not in pot_locs:
                continue
            ld = pot_locs[name]
            ld.can_reach = f"({ld.can_reach}){_soul_terms(souls)}"


def load_terrain(path: Path, logic_regions: dict[str, RegionDef] | None = None):
    """Load assets/rando/terrain.gen.yaml (local registry from
    gen_terrain_tables.py — add-rando-grass-rock-shuffle).

    Returns (name -> LocationDef, [(screen, pos, loc_id)]). Each terrain
    LocationDef carries id/name/region/type=Grass|Rock/can_reach so it feeds
    BOTH the location registry and the logic binding, exactly like pots.
    vanilla_item is always "Nothing": suppressed vanilla secrets are NOT pool
    items (design D8) and the ITEM_Nothing pre-pin in rando_placement.c is
    pot-type-scoped, so terrain rows are never pinned by it. The YAML is a
    gitignored local artifact: assetless builds emit no terrain locations and
    an empty lookup, and Phase-3/4 activation fails closed via the registry
    digest guard (kRandoTerrainRegistryDigest)."""
    if not path.exists():
        print(f"WARNING: {path} not found - emitting NO terrain locations. "
              f"Run zelda3 --dump-terrain-table + "
              f"assets/scripts/gen_terrain_tables.py --emit with ROM assets "
              f"to enable grass/rock shuffle in this build.", file=sys.stderr)
        return {}, []
    doc = load_yaml(path)
    out, rows = {}, []
    valid_regions = set(logic_regions or {})
    for t in doc.get("locations", []) or []:
        axis = t.get("axis")
        if axis not in ("grass", "rock"):
            sys.exit(f"{path}: row id={t.get('id')} has unknown axis {axis!r}")
        region = t.get("region", "")
        # Hard-fail an unresolvable region NAME (review MED-3). Terrain rows bind
        # by name to a logic region; a typo would resolve to region_id 0xFFFF,
        # which the runtime treats as "no region gate" (rando_logic.c) — an
        # UNDER-gate that lets the placer strand progression on an all-tier
        # object. The generic well-formedness warning is only fatal under
        # --strict, and CI's --strict pass runs WITHOUT the (gitignored) terrain
        # artifacts, so it never sees these rows — enforce here instead.
        if not region or (valid_regions and region not in valid_regions):
            sys.exit(f"{path}: row id={t.get('id')} ({t.get('name')}) binds to "
                     f"region {region!r} which is not a logic region — fix the "
                     f"SCREEN_REGIONS / terrain_logic_overrides binding "
                     f"(a 0xFFFF region skips the reachability gate = under-gate).")
        # world_state_filter (add-rando-grass-rock-shuffle world-group fix):
        # inverted rewrites the overworld, so a terrain object present-and-equal
        # in every NON-inverted latch state but absent/repurposed under inverted
        # (or vice versa) is registered for only its group's world_states. Empty
        # / absent = universal. Placement (rando_placement.c) AND reachability
        # (rando_logic.c) both honor the emitted mask, so a filtered-out object
        # is inert in the other world_state — no strand.
        wsf = t.get("world_state_filter") or []
        loc = LocationDef(
            id=t["id"], name=t["name"], region=region,
            type="Grass" if axis == "grass" else "Rock",
            vanilla_item="Nothing",
            can_reach=t.get("can_reach") or "TRUE()",
            can_place="TRUE()", always_allow="FALSE()",
            source="gen_terrain_tables.py",
            world_state_filter=list(wsf),
        )
        out[loc.name] = loc
        # Row carries the ws_mask so the registry digest covers world_state_filter
        # too (fresh-eyes review): the filter is placement-relevant registry
        # state, but it lives in the LOCATION table, not the lookup — without it
        # a filter-only registry change (same key set, different active
        # world_states) would keep an identical digest and slip past the runtime
        # drift guard. The lookup TABLE still emits only (screen,pos,loc_id).
        rows.append((int(t["screen"]), int(t["pos"]), int(t["id"]),
                     _world_state_mask(list(wsf))))
    return out, rows


# --- add-rando-ow-warp-shuffle — OW screen-component substrate ---------------
# Populated by load_ow_graph(); consumed by _region_ids_for_codegen so the
# warp regions are a LAST id-suffix group (the chain-boss precedent: appending
# after the alphabetical base + chain suffix keeps every existing region id
# stable, which the region_ids.lock.json guard enforces).
_OW_REGION_IDS: list[str] = []
_OW_REGION_ID_SET: set[str] = set()

# The 11 hand-written overworld surface zones (quotient domain). Membership is
# validated against the loaded graph at codegen time.
OW_SURFACE_ZONES = frozenset({
    "LightWorld_NorthWest", "LightWorld_NorthEast", "LightWorld_South",
    "LightWorld_DeathMountain_West", "LightWorld_DeathMountain_East",
    "DarkWorld_NorthWest", "DarkWorld_NorthEast", "DarkWorld_South",
    "DarkWorld_Mire", "DarkWorld_DeathMountain_West",
    "DarkWorld_DeathMountain_East",
})
OW_LW_SURFACE_ZONES = frozenset(z for z in OW_SURFACE_ZONES
                                if z.startswith("LightWorld"))
OW_FLUTE_HUB_REGION = "OW_FluteNet"


def _ow_region_id(component_name: str) -> str:
    """Component region id: prefixed + sanitized (upstream names carry spaces
    and parens; region ids elsewhere are identifier-ish)."""
    out = []
    for ch in component_name:
        out.append(ch if ch.isalnum() else "_")
    ident = "".join(out)
    while "__" in ident:
        ident = ident.replace("__", "_")
    return "OWC_" + ident.strip("_")


def _strip_canfly_conjunct(pred: str, ctx: str) -> str:
    """Remove the `CanFly(world)` conjunct from a teleporter zone-edge
    predicate (design D1: the zone edge conflates reaching the portal with
    operating it; verbatim reuse would compile false under an active flute
    shuffle). Hard-fails on shapes it cannot safely elide."""
    p = pred.strip()
    if "CanFly" not in p:
        return p
    for pat in (" AND CanFly(world)", "CanFly(world) AND "):
        if pat in p:
            out = p.replace(pat, "", 1).strip()
            if not out or "CanFly" in out:
                sys.exit(f"ow-graph: cannot safely strip CanFly from "
                         f"predicate {pred!r} ({ctx})")
            return out
    sys.exit(f"ow-graph: teleporter predicate {pred!r} ({ctx}) is not a "
             f"conjunction CanFly can be elided from")


def load_ow_graph(path: Path,
                  logic_regions: dict[str, "RegionDef"],
                  logic_edges: list["EdgeDef"]):
    """Load assets/rando/ow_graph.gen.yaml (gitignored, from
    gen_ow_graph_tables.py — add-rando-ow-warp-shuffle).

    Returns (ow_regions, ow_edges, meta). ow_regions/ow_edges are APPENDED by
    the caller after everything else so components form the trailing region-id
    suffix and warp edges the trailing kRandoEdges suffix (the reachability
    walker bounds its edge scan at kRandoNonWarpEdgesCount when no warp axis
    is active). Absent file: WARNING + empty tables (the terrain precedent —
    assetless builds emit no warp graph and generation/activation fail closed
    via kRandoOwGraphPresent/digest guards)."""
    meta = {"present": 0, "digest": 0, "flute": [], "whirlpools": [],
            "vanilla_flute": [], "hub_region": None, "sectors": []}
    if not path.exists():
        print(f"WARNING: {path} not found - emitting NO ow-warp graph. "
              f"Run assets/scripts/gen_ow_graph_tables.py (needs the "
              f"ALttPDoorRandomizer sibling checkout) to enable "
              f"flute/whirlpool shuffle in this build.", file=sys.stderr)
        return {}, [], meta
    doc = load_yaml(path)
    comps = doc.get("components", []) or []
    if not comps:
        sys.exit(f"{path}: components list empty")
    digest = doc.get("identity_digest")
    if not isinstance(digest, int):
        sys.exit(f"{path}: identity_digest missing")

    by_name = {}
    ow_regions: dict[str, RegionDef] = {}
    for c in comps:
        name, zone = c["name"], c["zone"]
        if zone not in logic_regions:
            sys.exit(f"{path}: component {name!r} zone {zone!r} is not a "
                     f"logic region")
        if zone not in OW_SURFACE_ZONES:
            sys.exit(f"{path}: component {name!r} zone {zone!r} not an OW "
                     f"surface zone")
        rid = _ow_region_id(name)
        if rid in by_name.values() or rid in logic_regions:
            sys.exit(f"{path}: region id collision for {name!r} -> {rid}")
        by_name[name] = rid
        ow_regions[rid] = RegionDef(
            id=rid, name=f"OW: {name}", parent=zone,
            source="ow_graph.gen.yaml")

    hub = RegionDef(id=OW_FLUTE_HUB_REGION, name="OW flute network",
                    source="ow_graph.gen.yaml")
    ow_regions[OW_FLUTE_HUB_REGION] = hub

    ow_edges: list[EdgeDef] = []

    def edge(f, t, pred, one_way=True, why=""):
        ow_edges.append(EdgeDef(from_=f, to=t, predicate=pred,
                                one_way=one_way,
                                source=f"ow_graph:{why}"))

    # comp->zone (unconditional) for every non-stub component.
    stubs = {c["name"] for c in comps if c.get("stub")}
    for c in comps:
        if c["name"] not in stubs:
            edge(by_name[c["name"]], c["zone"], "TRUE()", why="egress")

    # zone->whirlpool-water entry (Flippers).
    wps = doc.get("whirlpools", []) or []
    wp_names = set()
    for wp in wps:
        for key in ("component", "partner_component"):
            n = wp[key]
            if n in wp_names:
                continue
            wp_names.add(n)
            if n in stubs:
                sys.exit(f"{path}: whirlpool water {n!r} is a stub")
            zone = next(c["zone"] for c in comps if c["name"] == n)
            edge(zone, by_name[n], "HAS_ITEM(Flippers)", why="whirlpool-entry")

    # directed drops.
    for src, dst in (doc.get("drops", []) or []):
        edge(by_name[src], by_name[dst], "TRUE()", why="drop")

    # portal edges (flute-candidate hosts; CanFly-stripped predicate reuse).
    _portal_pairs = []
    flute = doc.get("flute_candidates", []) or []
    cand_comps = {e["component"] for e in flute}
    ow_zone_edges = [e for e in logic_edges
                     if e.from_ in OW_SURFACE_ZONES and
                     e.to in OW_SURFACE_ZONES]
    for t in (doc.get("teleporters", []) or []):
        if t["host"] not in cand_comps:
            continue
        tz, hz = t["target_zone"], t["host_zone"]
        # Predicate reuse is TARGET-keyed: prefer the edge from the host's
        # own zone, then the flute-carried edge (the Desert/Mire class),
        # then a single unambiguous edge into the target (the host's screen
        # may be zone-bound differently than the edge's from-zone — the
        # terrain screen map and the hand-written graph partition DM screens
        # differently). Multiple ambiguous candidates = hard error.
        cands = [e for e in ow_zone_edges if e.to == tz]
        pick = next((e for e in cands if e.from_ == hz), None) or \
            next((e for e in cands if "CanFly" in e.predicate), None) or \
            (cands[0] if len(cands) == 1 else None)
        if pick is None:
            sys.exit(f"{path}: no unambiguous zone edge into {tz!r} to "
                     f"derive the portal predicate for teleporter "
                     f"{t['name']!r} (candidates: "
                     f"{[(e.from_, e.predicate) for e in cands]})")
        pred = _strip_canfly_conjunct(pick.predicate, t["name"])
        edge(by_name[t["host"]], tz, pred, why=f"portal:{t['name']}")
        _portal_pairs.append((ow_edges[-1], hz))

    # hub feeders: LW surface zones -> hub, activation-only macro (never
    # CanFly — its neutralization would dead-end the hub; round-2 H1).
    for z in sorted(OW_LW_SURFACE_ZONES):
        edge(z, OW_FLUTE_HUB_REGION, "CanUseFluteTravel(world)",
             why="flute-hub")

    # NOTE: hub->spot edges are PER-SEED (installed through the added-edge
    # overlay by shuffle_ow_warp.c), never static.

    _portal_zone_pairs = sorted({(zone_of_host, e.to)
                                 for e, zone_of_host in _portal_pairs})
    _drop_zone_pairs = sorted({
        (next(c["zone"] for c in comps if c["name"] == s),
         next(c["zone"] for c in comps if c["name"] == d))
        for s, d in (doc.get("drops", []) or [])
        if next(c["zone"] for c in comps if c["name"] == s) !=
        next(c["zone"] for c in comps if c["name"] == d)})
    meta.update({
        "portal_zone_pairs": _portal_zone_pairs,
        "drop_zone_pairs": _drop_zone_pairs,
        "present": 1, "digest": digest & 0xFFFFFFFF,
        "flute": [{**e, "region_id": by_name[e["component"]]} for e in flute],
        "whirlpools": [{**wp,
                        "region_id": by_name[wp["component"]],
                        "partner_region_id": by_name[wp["partner_component"]]}
                       for wp in wps],
        "vanilla_flute": doc.get("vanilla_flute_screens", []) or [],
        "hub_region": OW_FLUTE_HUB_REGION,
        "sectors": doc.get("sectors", []) or [],
        "adjacency_cross_zone": doc.get("adjacency_cross_zone", []) or [],
        "comp_zone": {c["name"]: c["zone"] for c in comps},
    })
    global _OW_REGION_IDS, _OW_REGION_ID_SET
    _OW_REGION_IDS = list(ow_regions.keys())
    _OW_REGION_ID_SET = set(_OW_REGION_IDS)
    return ow_regions, ow_edges, meta


def ow_quotient_check(meta, logic_edges, locations, allowlist_path: Path,
                      all_errors: list):
    """Design D1's quotient cross-check: collapse the component graph by zone
    and reconcile against the hand-written OW zone edges, both directions,
    through a committed carrier-validated allowlist."""
    if not meta["present"]:
        return
    zone_of = meta["comp_zone"]
    derived = set()
    for a, b in meta["adjacency_cross_zone"]:
        derived.add((zone_of[a], zone_of[b]))
        derived.add((zone_of[b], zone_of[a]))
    # Portal and drop edges are part of the derived graph too — a hand
    # zone edge realized by a teleporter or a ledge drop is witnessed.
    for pair in meta.get("portal_zone_pairs", []):
        derived.add(tuple(pair))
    for pair in meta.get("drop_zone_pairs", []):
        derived.add(tuple(pair))
    allow = {}
    if allowlist_path.exists():
        for row in (load_yaml(allowlist_path).get("allow", []) or []):
            allow[(row["from"], row["to"])] = row
            carrier = row.get("carrier", "")
            ok = False
            if carrier.startswith("predicate:"):
                token = carrier.split(":", 1)[1]
                ok = any(token in e.predicate for e in logic_edges
                         if {e.from_, e.to} == {row["from"], row["to"]} or
                         token in e.predicate and
                         (e.from_ in (row["from"], row["to"]) or
                          e.to in (row["from"], row["to"])))
            elif carrier.startswith("location:"):
                ok = carrier.split(":", 1)[1] in locations
            elif carrier.startswith("mechanism:"):
                ok = carrier.split(":", 1)[1] in ("mirror", "flute",
                                                  "whirlpool", "portal")
            elif carrier.startswith("conservative:"):
                # Reviewed deliberate omission: upstream witnesses the
                # adjacency but the fork graph intentionally lacks the edge
                # (item-gated link kept predicate-side, or connectivity
                # redundant by hub construction). Requires a non-empty
                # reviewed note; the UNDER-grant direction is placement-safe.
                ok = len(carrier.split(":", 1)[1].strip()) > 0
            if not ok:
                all_errors.append(
                    f"ow_quotient_allowlist: entry {row['from']}->{row['to']}"
                    f" carrier {carrier!r} does not resolve")
    hand = set()
    for e in logic_edges:
        if e.from_ in OW_SURFACE_ZONES and e.to in OW_SURFACE_ZONES:
            hand.add((e.from_, e.to))
            if not e.one_way:
                hand.add((e.to, e.from_))
    for pair in sorted(derived):
        if pair not in hand and pair not in allow:
            all_errors.append(
                f"ow-quotient: derived zone adjacency {pair[0]}->{pair[1]} "
                f"has no hand-written zone edge and no allowlist entry")
    for pair in sorted(hand):
        if pair not in derived and pair not in allow:
            all_errors.append(
                f"ow-quotient: hand-written zone edge {pair[0]}->{pair[1]} "
                f"is unwitnessed by component adjacency and not allowlisted")


def load_bonk(path: Path, logic_regions: dict[str, RegionDef] | None = None):
    """Load assets/rando/bonk.gen.yaml (local registry from
    gen_bonk_tables.py — add-rando-bonk-sanity).

    Returns (name -> LocationDef, [(area, block, loc_id)]). Mirrors
    load_terrain: vanilla_item is always "Nothing" (a suppressed vanilla wake
    is not a pool item), the region binding hard-fails on an unresolvable
    name (a 0xFFFF region would UNDER-gate), and an absent artifact emits no
    rows — active bonk generation then fails closed via the registry
    digest/count guard (kRandoBonkRegistryDigest)."""
    if not path.exists():
        print(f"WARNING: {path} not found - emitting NO bonk locations. "
              f"Run assets/scripts/gen_bonk_tables.py with ROM assets to "
              f"enable bonk shuffle in this build.", file=sys.stderr)
        return {}, []
    doc = load_yaml(path)
    out, rows = {}, []
    valid_regions = set(logic_regions or {})
    for t_ in doc.get("locations", []) or []:
        region = t_.get("region", "")
        if not region or (valid_regions and region not in valid_regions):
            sys.exit(f"{path}: row id={t_.get('id')} ({t_.get('name')}) binds "
                     f"to region {region!r} which is not a logic region — fix "
                     f"OVERWORLD_REGIONS / bonk_logic_overrides (a 0xFFFF "
                     f"region skips the reachability gate = under-gate).")
        loc = LocationDef(
            id=t_["id"], name=t_["name"], region=region,
            type="Bonk",
            vanilla_item="Nothing",
            can_reach=t_.get("can_reach") or "TRUE()",
            can_place="TRUE()", always_allow="FALSE()",
            source="gen_bonk_tables.py",
            world_state_filter=[],
        )
        out[loc.name] = loc
        rows.append((int(t_["area"]), int(t_["block"]), int(t_["id"])))
    return out, rows


def load_enemy_drops(path: Path, logic_regions: dict[str, RegionDef] | None = None):
    """Load assets/rando/enemy_drops.gen.yaml.

    The file is local/ROM-derived and optional, like pots.gen.yaml. When absent
    public builds emit no EnemyDrop locations plus an empty runtime lookup; an
    active enemy_drop_checks seed fails closed in the placer.
    """
    if not path.exists():
        print(f"WARNING: {path} not found - emitting NO enemy-drop locations. "
              f"Run assets/scripts/gen_enemy_drop_tables.py with ROM assets "
              f"to enable enemy-drop checks in this build.", file=sys.stderr)
        return {}, [], [], [], []
    doc = load_yaml(path)
    if int(doc.get("format_version", 0) or 0) != 3:
        raise RuntimeError(f"{path}: unsupported format_version {doc.get('format_version')!r}")
    source_counts = _enemy_source_counts(doc)
    pot_key_counts = _load_pot_key_counts_by_item()
    out, rows, door_bridge_rows = {}, [], []
    seen = set()
    seen_rooms = set()
    big_key_rows = 0
    for raw in doc.get("enemy_drops", []) or []:
        drop_kind_raw = raw.get("drop")
        if drop_kind_raw not in ("small_key", "big_key"):
            raise RuntimeError(f"{path}: unsupported enemy_drop kind {drop_kind_raw!r}")
        if drop_kind_raw == "big_key":
            big_key_rows += 1
            if raw.get("big_key_policy") != "one_shot_preserve_vanilla_big_key":
                raise RuntimeError(f"{path}: big_key enemy_drop lacks the reviewed one-shot policy")
            if raw.get("vanilla_item") != "Nothing":
                raise RuntimeError(f"{path}: one-shot big_key enemy_drop must use vanilla_item Nothing")
            for field in ("small_key_item", "key_depth", "key_mindepth"):
                if field not in raw:
                    raise RuntimeError(f"{path}: big_key enemy_drop lacks {field}; regenerate with gen_enemy_drop_tables.py")
        else:
            for field in ("door_drop_index", "key_depth", "key_mindepth"):
                if field not in raw:
                    raise RuntimeError(f"{path}: small_key enemy_drop lacks {field}; regenerate with gen_enemy_drop_tables.py")
                if int(raw[field]) < 0:
                    raise RuntimeError(f"{path}: small_key enemy_drop has negative {field}")
        room = int(raw["room"])
        source_slot = int(raw["source_slot"])
        key = (room, source_slot)
        if key in seen:
            raise RuntimeError(f"{path}: duplicate enemy-drop source room=0x{room:03x} slot={source_slot}")
        seen.add(key)
        if room in seen_rooms:
            raise RuntimeError(f"{path}: multiple active enemy-drop keys in room 0x{room:03x}")
        seen_rooms.add(room)
        door_dungeon = int(raw["door_dungeon"])
        door_region = int(raw["door_region"])
        if door_dungeon < 0 or door_region < 0:
            raise RuntimeError(f"{path}: negative door identity for enemy-drop room 0x{room:03x}")
        base_can_reach = raw.get("can_reach") or "CanKillMostThings(world, 5)"
        can_reach = base_can_reach
        if drop_kind_raw == "small_key":
            wild, dungeon, combined_wild = _enemy_depth_terms(
                raw, raw["vanilla_item"], source_counts, pot_key_counts)
            can_reach = _suppress_base_key_terms_under_enemy_dungeon(
                can_reach, raw["vanilla_item"], dungeon)
            terms = _enemy_drop_key_terms_for(
                raw["vanilla_item"], wild, dungeon, combined_wild)
            if terms:
                can_reach = f"({can_reach}){terms}"
            door_pred = _strip_door_vanilla_key_terms(
                base_can_reach, door_dungeon)
            door_bridge_rows.append({
                "loc_id": int(raw["id"]),
                "name": raw["name"],
                "dungeon": door_dungeon,
                "region": door_region,
                "drop_index": int(raw["door_drop_index"]),
                "base_can_reach": door_pred,
            })
        elif drop_kind_raw == "big_key":
            small_key_item = raw["small_key_item"]
            wild, dungeon, combined_wild = _enemy_depth_terms(
                raw, small_key_item, source_counts, pot_key_counts)
            can_reach = _suppress_base_key_terms_under_enemy_dungeon(
                can_reach, small_key_item, dungeon)
            terms = _enemy_drop_key_terms_for(
                small_key_item, wild, dungeon, combined_wild)
            if terms:
                can_reach = f"({can_reach}){terms}"
        loc = LocationDef(
            id=int(raw["id"]),
            name=raw["name"],
            region=raw["region"],
            type="EnemyDrop",
            vanilla_item=raw["vanilla_item"],
            can_reach=can_reach,
            can_place=raw.get("can_place") or "TRUE()",
            always_allow=raw.get("always_allow") or "FALSE()",
            source="gen_enemy_drop_tables.py",
        )
        if logic_regions is not None and loc.region not in logic_regions:
            raise RuntimeError(f"{path}: enemy-drop location {loc.name!r} uses unknown region {loc.region!r}")
        out[loc.name] = loc
        rows.append({
            "room": room,
            "source_slot": source_slot,
            "loc_id": loc.id,
            "vanilla_item": loc.vanilla_item,
            "drop_kind": drop_kind_raw,
            "door_dungeon": door_dungeon,
            "door_region": door_region,
        })
    if not (doc.get("big_key_candidates") or []):
        raise RuntimeError(f"{path}: missing recorded big_key_candidates")
    if big_key_rows != 1:
        raise RuntimeError(f"{path}: expected exactly one one-shot big_key enemy_drop row")
    key_depth_rows = []
    for raw in doc.get("key_depth_locations", []) or []:
        item = raw["item"]
        wild, dungeon, combined_wild = _enemy_depth_terms(raw, item, source_counts, pot_key_counts)
        key_depth_rows.append({
            "name": raw["name"],
            "item": item,
            "wild": wild,
            "dungeon": dungeon,
            "combined_wild": combined_wild,
        })
    return out, rows, door_bridge_rows, key_depth_rows, (doc.get("hce_big_key_locations") or [])


def load_enemy_checks(path: Path, logic_regions: dict[str, RegionDef] | None = None,
                      enemy_drop_source_counts: dict[str, dict[str, int]] | None = None):
    """Load assets/rando/enemy_checks.gen.yaml.

    Ordinary enemy checks are separate from forced EnemyDrop key rows. Missing
    local artifacts emit no Enemy locations plus empty runtime lookups; active
    enemy_drop_checks=dungeon/all seeds fail closed in the placer.
    """
    if not path.exists():
        print(f"WARNING: {path} not found - emitting NO ordinary enemy-check "
              f"locations. Run assets/scripts/gen_enemy_check_tables.py with "
              f"ROM assets to enable enemy_drop_checks=dungeon in this build.",
              file=sys.stderr)
        return {}, [], []
    if logic_regions is None:
        raise RuntimeError(f"{path}: logic_regions are required for enemy-check region resolution")
    doc = load_yaml(path)
    if int(doc.get("format_version", 0) or 0) != 1:
        raise RuntimeError(f"{path}: unsupported format_version {doc.get('format_version')!r}")
    pot_key_counts = _load_pot_key_counts_by_item()
    source_counts = enemy_drop_source_counts or {}
    out, rows, door_bridge_rows = {}, [], []
    seen = set()
    for raw in doc.get("enemy_checks", []) or []:
        if raw.get("type") != "Enemy":
            raise RuntimeError(f"{path}: ordinary enemy row {raw.get('name')!r} must use type Enemy")
        domain = raw.get("domain") or "dungeon"
        if domain == "dungeon":
            source_slot = int(raw["source_slot"])
            room = int(raw["room"])
            key = (domain, room, source_slot)
        elif domain == "overworld":
            source_slot = int(raw["source_slot"])
            area = int(raw["area"])
            stage = int(raw["stage"])
            block = int(raw["block"])
            key = (domain, area, stage, source_slot, block)
        elif domain == "boss":
            room = int(raw["room"])
            key = (
                domain,
                raw.get("boss_kind", ""),
                int(raw.get("game_dungeon", 0xFF)),
                room,
            )
        elif domain == "scripted_spawn":
            room = int(raw["room"])
            key = (
                domain,
                room,
                int(raw["parent_source_slot"]),
                int(raw["overlord_type"]),
                int(raw["child_index"]),
                int(raw["child_type"]),
            )
        else:
            raise RuntimeError(f"{path}: enemy-check row {raw['name']!r} uses unsupported domain {domain!r}")
        if key in seen:
            raise RuntimeError(f"{path}: duplicate enemy-check source key={key!r}")
        seen.add(key)
        region_name = raw.get("region")
        if not region_name or region_name not in logic_regions:
            raise RuntimeError(f"{path}: enemy-check row {raw['name']!r} uses unknown region {region_name!r}")
        base_can_reach = raw.get("can_reach") or "TRUE()"
        can_reach = base_can_reach
        all_tier_only = bool(raw.get("all_tier_only", False))
        if domain in ("dungeon", "scripted_spawn"):
            if "small_key_item" in raw:
                item = raw["small_key_item"]
                wild, dungeon, combined_wild = _enemy_depth_terms(raw, item, source_counts, pot_key_counts)
                can_reach = _suppress_base_key_terms_under_enemy_dungeon(
                    base_can_reach, item, dungeon)
                terms = _enemy_drop_key_terms_for(item, wild, dungeon, combined_wild)
                if terms:
                    can_reach = f"({can_reach}){terms}"
            elif domain == "dungeon" and not all_tier_only:
                raise RuntimeError(
                    f"{path}: dungeon enemy-check row {raw['name']!r} lacks small_key_item")
        loc = LocationDef(
            id=int(raw["id"]),
            name=raw["name"],
            region=region_name,
            type="Enemy",
            vanilla_item=raw.get("vanilla_item") or "Nothing",
            can_reach=can_reach,
            can_place=raw.get("can_place") or "TRUE()",
            always_allow=raw.get("always_allow") or "FALSE()",
            source="gen_enemy_check_tables.py",
        )
        if loc.vanilla_item != "Nothing":
            raise RuntimeError(f"{path}: ordinary enemy-check row {loc.name!r} must use vanilla_item Nothing")
        out[loc.name] = loc
        if domain == "dungeon":
            rows.append({
                "domain": "dungeon",
                "room": room,
                "source_slot": source_slot,
                "loc_id": loc.id,
                "all_tier_only": all_tier_only,
            })
            if "door_dungeon" in raw and "door_region" in raw:
                door_regions = [int(x) for x in raw.get("door_regions", []) or []]
                if not door_regions:
                    door_regions = [int(raw["door_region"])]
                door_pred = raw.get("base_can_reach") or base_can_reach
                door_pred = _strip_door_vanilla_key_terms(
                    door_pred, int(raw["door_dungeon"]))
                door_bridge_rows.append({
                    "loc_id": loc.id,
                    "name": raw["name"],
                    "dungeon": int(raw["door_dungeon"]),
                    "regions": sorted(set(door_regions)),
                    "base_can_reach": door_pred,
                    "min_tier": 3 if all_tier_only else 2,
                })
        elif domain == "boss":
            rows.append({
                "domain": "boss",
                "boss_kind": raw.get("boss_kind", ""),
                "game_dungeon": int(raw["game_dungeon"]),
                "room": room,
                "loc_id": loc.id,
            })
            if "door_dungeon" in raw and "door_region" in raw:
                door_dungeon = int(raw["door_dungeon"])
                door_bridge_rows.append({
                    "loc_id": loc.id,
                    "name": raw["name"],
                    "dungeon": door_dungeon,
                    "regions": [int(raw["door_region"])],
                    "base_can_reach": _strip_door_vanilla_key_terms(
                        raw.get("base_can_reach") or base_can_reach,
                        door_dungeon),
                    "min_tier": 3,
                })
        elif domain == "scripted_spawn":
            rows.append({
                "domain": "scripted_spawn",
                "room": room,
                "parent_source_slot": int(raw["parent_source_slot"]),
                "overlord_type": int(raw["overlord_type"]),
                "child_index": int(raw["child_index"]),
                "child_type": int(raw["child_type"]),
                "loc_id": loc.id,
            })
            if "door_dungeon" in raw and "door_region" in raw:
                door_regions = [int(x) for x in raw.get("door_regions", []) or []]
                if not door_regions:
                    door_regions = [int(raw["door_region"])]
                door_pred = raw.get("base_can_reach") or base_can_reach
                door_pred = _strip_door_vanilla_key_terms(
                    door_pred, int(raw["door_dungeon"]))
                door_bridge_rows.append({
                    "loc_id": loc.id,
                    "name": raw["name"],
                    "dungeon": int(raw["door_dungeon"]),
                    "regions": sorted(set(door_regions)),
                    "base_can_reach": door_pred,
                    "min_tier": 3,
                })
        else:
            rows.append({
                "domain": "overworld",
                "area": area,
                "stage": stage,
                "source_slot": source_slot,
                "block": block,
                "loc_id": loc.id,
                "all_tier_only": all_tier_only,
            })
    return out, rows, door_bridge_rows


def load_cave_source_predicates(path: Path) -> list[tuple[str, str]]:
    """Return entrance-shuffle cave source predicates in registry order."""
    if not path.exists():
        return []
    doc = load_yaml(path) or {}
    rows = []
    for interior in doc.get("interiors", []) or []:
        rows.append((
            str(interior.get("interior_id", "")),
            str(interior.get("can_enter") or "TRUE()"),
        ))
    return rows


def build_door_pot_bridge(bridge_sources: list[dict], compile_src) -> tuple[list[dict], int]:
    rows: list[dict] = []
    for src in sorted(bridge_sources, key=lambda r: r["loc_id"]):
        pred = compile_src(src["base_can_reach"],
                           f"pot bridge {src['loc_id']} {src['name']}")
        row = dict(src)
        row["pred"] = pred
        rows.append(row)
    return rows, _door_pot_bridge_digest(rows) if rows else 0


def build_door_enemy_drop_bridge(bridge_sources: list[dict], compile_src) -> tuple[list[dict], int]:
    rows: list[dict] = []
    for src in sorted(bridge_sources, key=lambda r: r["loc_id"]):
        pred = compile_src(src["base_can_reach"],
                           f"enemy drop bridge {src['loc_id']} {src['name']}")
        row = dict(src)
        row["pred"] = pred
        rows.append(row)
    return rows, _door_enemy_drop_bridge_digest(rows) if rows else 0


def build_door_enemy_check_bridge(bridge_sources: list[dict], compile_src) -> tuple[list[dict], int]:
    rows: list[dict] = []
    for src in sorted(bridge_sources, key=lambda r: r["loc_id"]):
        pred = compile_src(src["base_can_reach"],
                           f"enemy check bridge {src['loc_id']} {src['name']}")
        row = dict(src)
        row["pred"] = pred
        rows.append(row)
    return rows, _door_enemy_check_bridge_digest(rows) if rows else 0


def load_macros(path: Path | None) -> dict[str, MacroDef]:
    if path is None or not path.exists():
        return {}
    doc = load_yaml(path)
    out = {}
    for raw in doc.get("macros", []):
        m = MacroDef(
            name=raw["name"],
            parameters=raw.get("parameters", []) or [],
            body=raw["body"],
            source=raw.get("source", ""),
        )
        out[m.name] = m
    return out


def _merge_logic_doc(doc, regions, edges, loc_preds, macros, source_file,
                     world_state_overrides=None, world_state_id=None,
                     world_state_edges=None):
    """Merge a single parsed logic YAML doc into the cumulative dicts.

    Per-file conflict policy: regions / macros are keyed by id/name; a later
    file silently overrides an earlier one (the codegen reports duplicates as
    warnings via well-formedness). Edges are appended. Location predicates
    are keyed by id; later wins.

    Phase B Slice 2 (add-rando-inverted-world-state): when ``world_state_id``
    is not None, location predicates and edges from this file are stored in
    ``world_state_overrides[loc_id][world_state_id]`` and
    ``world_state_edges[world_state_id]`` instead of the base maps. The base
    ``loc_preds`` / ``edges`` only receive entries from files with
    ``world_state_id is None`` (i.e., the Standard/Open/Retro path).
    Regions and macros still merge into the global maps regardless of
    world_state (they're identity-shared across world states by design).
    """
    if doc is None:
        return
    for raw in doc.get("regions", []) or []:
        r = RegionDef(
            id=raw["id"],
            name=raw.get("name", raw["id"]),
            dungeon=raw.get("dungeon"),
            parent=raw.get("parent"),
            world_state_filter=raw.get("world_state_filter", []),
            source=raw.get("source", ""),
        )
        # Don't let a world-state-specific YAML overwrite a base region
        # with a "stub" entry that would lose the parent / dungeon
        # binding. Only insert if not already present.
        if world_state_id is None or r.id not in regions:
            regions[r.id] = r
    for raw in doc.get("edges", []) or []:
        edge = EdgeDef(
            from_=raw["from"],
            to=raw["to"],
            predicate=raw.get("predicate", "TRUE()"),
            one_way=raw.get("one_way", False),
            source=raw.get("source", ""),
        )
        if world_state_id is None:
            edges.append(edge)
        else:
            world_state_edges.setdefault(world_state_id, []).append(edge)
    for raw in doc.get("locations", []) or []:
        loc = LocationDef(
            id=0,  # numeric id comes from registry
            name=raw.get("name", raw["id"]),
            region=raw.get("region", ""),
            type=raw.get("type", "Chest"),
            vanilla_item=raw.get("vanilla_item", ""),
            can_reach=raw.get("can_reach", "TRUE()"),
            can_place=raw.get("can_place", "TRUE()"),
            always_allow=raw.get("always_allow", "FALSE()"),
            source=raw.get("source", ""),
        )
        if world_state_id is None:
            loc_preds[raw["id"]] = loc
        else:
            world_state_overrides.setdefault(raw["id"], {})[world_state_id] = loc
    for raw in doc.get("macros", []) or []:
        macros[raw["name"]] = MacroDef(
            name=raw["name"],
            parameters=raw.get("parameters", []) or [],
            body=raw["body"],
            source=raw.get("source", ""),
        )


# Phase B Slice 2 — world-state numeric IDs matching the C-side enum
# (src/rando/rando_settings.h `kWorldState_*`). Used to bucket YAML files
# from `logic_parts/inverted/**` into the Inverted override map.
kWorldState_Open = 0
kWorldState_Standard = 1
kWorldState_Inverted = 2
kWorldState_Retro = 3
_WORLD_STATE_NAME_BY_ID = {
    kWorldState_Open: "open",
    kWorldState_Standard: "standard",
    kWorldState_Inverted: "inverted",
    kWorldState_Retro: "retro",
}


def _world_state_id_for_path(path: Path) -> int | None:
    """Derive the world_state override key from a logic_parts file path.

    `logic_parts/inverted/**/*.yaml` → kWorldState_Inverted
    `logic_parts/*.yaml`              → None (base / Standard / shared across
                                              all world states)

    Future world-state-specific subdirs (e.g., `logic_parts/retro/`) plug
    into the same convention.
    """
    parts = path.parts
    if "inverted" in parts:
        return kWorldState_Inverted
    # Future: if "retro" in parts: return kWorldState_Retro
    return None


def load_logic(path: Path | None):
    """Load the optional logic.yaml plus every file under
    `assets/rando/logic_parts/**/*.yaml` (sorted for deterministic merge order).
    Returns (regions, edges, location_predicates, macros, world_state_overrides, world_state_edges).

    Files later in sort order override earlier files for regions / locations /
    macros at the BASE level. Phase B Slice 2: files under
    `logic_parts/inverted/**` are routed to the `world_state_overrides`
    map keyed by location id → {world_state_id: LocationDef}. The base
    `loc_preds` only holds Standard/Open/Retro predicates. The runtime
    consults the override map when `settings.world_state == Inverted`.

    Cross-region edges from Inverted YAML go into `world_state_edges` and
    are emitted as Inverted-specific edges in `logic_data.c`. The base
    `edges` list is unchanged.
    """
    regions: dict[str, RegionDef] = {}
    edges: list[EdgeDef] = []
    loc_preds: dict[str, LocationDef] = {}
    macros: dict[str, MacroDef] = {}
    # location_id → {world_state_id: LocationDef}
    world_state_overrides: dict[str, dict[int, LocationDef]] = {}
    # world_state_id → list[EdgeDef]
    world_state_edges: dict[int, list[EdgeDef]] = {}

    # Load the main logic.yaml first (lowest priority).
    if path is not None and path.exists():
        _merge_logic_doc(
            load_yaml(path), regions, edges, loc_preds, macros, str(path),
            world_state_overrides=world_state_overrides,
            world_state_id=None,
            world_state_edges=world_state_edges,
        )

    # Load every file under assets/rando/logic_parts/**/*.yaml (sorted).
    # Phase B Slice 2: recurse so `inverted/**/*.yaml` is picked up; route
    # those entries to the world_state_overrides / world_state_edges maps
    # via `_world_state_id_for_path`.
    parts_dir = RANDO_ASSETS / "logic_parts"
    if parts_dir.exists() and parts_dir.is_dir():
        for part in sorted(parts_dir.rglob("*.yaml")):
            ws_id = _world_state_id_for_path(part)
            _merge_logic_doc(
                load_yaml(part), regions, edges, loc_preds, macros, str(part),
                world_state_overrides=world_state_overrides,
                world_state_id=ws_id,
                world_state_edges=world_state_edges,
            )

    _apply_pot_key_terms(loc_preds, world_state_overrides)
    _apply_chain_boss_room_factoring(
        regions, edges, loc_preds, world_state_overrides)
    return regions, edges, loc_preds, macros, world_state_overrides, world_state_edges


def _apply_pot_key_terms(loc_preds, world_state_overrides):
    """add-rando-pot-sanity task #25: wrap each pot-bearing dungeon location's
    can_reach with the small-key requirements pot_shuffle adds — the WILD
    worst-case (`full`, held externally) and the in-context DUNGEON worst-case
    (`dungeon`). The dungeon term matters because the vanilla `cur` assumes the
    pot keys drop FREE; once they are items the requirement RISES to the
    conservative all-orders depth.
    pots-off leaves both POT_KEYS_WILD and POT_KEYS_DUNGEON false, so the wrap
    collapses to the vanilla predicate (byte-identical).

    Driven by the generated assets/rando/pot_key_depth.gen.yaml (from
    `./zelda3 --dump-key-depth`); applies to the base Standard/Open/Retro
    predicate AND every world-state (Inverted) override of the same location id
    (the internal door depth is world-state-independent)."""
    table_path = RANDO_ASSETS / "pot_key_depth.gen.yaml"
    if not table_path.exists():
        # This gitignored local artifact is absent in public/assetless builds,
        # which also omit pots.gen.yaml and skip pot-shuffle corpus rows. A
        # partial local build with pots.gen.yaml present fails closed in
        # load_pots() below so pot-shuffle binaries cannot silently miss these
        # wraps.
        print(f"WARNING: {table_path} MISSING — pot-key depth wraps NOT applied; "
              f"pot_shuffle key-mode seeds may strand. Regenerate with "
              f"gen_pot_key_depth.py.", file=sys.stderr)
        return
    doc = load_yaml(table_path)
    missing = []
    for row in doc.get("locations", []):
        name, item = row["name"], row["item"]
        full = int(row["full"]) if "full" in row else None
        dungeon = int(row["dungeon"]) if "dungeon" in row else None
        tail = _pot_key_terms_for(item, full, dungeon)
        if not tail and dungeon is None:
            continue
        applied = False
        if name in loc_preds:
            loc_preds[name].can_reach = _suppress_base_key_terms_under_dungeon(
                loc_preds[name].can_reach, item, dungeon)
            if tail:
                loc_preds[name].can_reach = f"({loc_preds[name].can_reach}){tail}"
            applied = True
        for ld in world_state_overrides.get(name, {}).values():
            ld.can_reach = _suppress_base_key_terms_under_dungeon(
                ld.can_reach, item, dungeon)
            if tail:
                ld.can_reach = f"({ld.can_reach}){tail}"
            applied = True
        if not applied:
            missing.append(name)
    if missing:
        sample = ", ".join(repr(n) for n in missing[:8])
        if len(missing) > 8:
            sample += f", ... (+{len(missing) - 8} more)"
        raise RuntimeError(
            f"{table_path}: pot_key_depth location(s) match no logic location: "
            f"{sample}. Regenerate or fix the stale table before codegen."
        )


def _is_can_kill_boss_call(ast, dungeon: str) -> bool:
    if not isinstance(ast, tuple):
        return False
    if ast[0] not in ("call", "macro", "op"):
        return False
    name = ast[1]
    args = ast[2]
    if ast[0] == "op":
        if name != "CAN_KILL_BOSS":
            return False
    elif name != "CanKillBoss":
        return False
    return len(args) == 1 and args[0] == ("ident", dungeon)


def _count_can_kill_boss_calls(ast, dungeon: str) -> int:
    if not isinstance(ast, tuple):
        return 0
    if _is_can_kill_boss_call(ast, dungeon):
        return 1
    kind = ast[0]
    if kind in ("and", "or"):
        return sum(_count_can_kill_boss_calls(child, dungeon) for child in ast[1])
    if kind == "not":
        return _count_can_kill_boss_calls(ast[1], dungeon)
    if kind == "cmp":
        return (_count_can_kill_boss_calls(ast[2], dungeon) +
                _count_can_kill_boss_calls(ast[3], dungeon))
    return 0


def _flatten_and(ast) -> list:
    if isinstance(ast, tuple) and ast[0] == "and":
        out = []
        for child in ast[1]:
            out.extend(_flatten_and(child))
        return out
    return [ast]


def _arg_to_predicate_source(arg) -> str:
    if isinstance(arg, tuple):
        if arg[0] == "ident":
            return arg[1]
        if arg[0] == "int":
            return str(arg[1])
        if arg[0] == "list":
            return "[" + ", ".join(arg[1]) + "]"
    if isinstance(arg, int):
        return str(arg)
    if isinstance(arg, str):
        return arg
    raise ParseError(f"cannot stringify predicate argument {arg!r}")


def _ast_to_predicate_source(ast, parent_prec: int = 0) -> str:
    if not isinstance(ast, tuple):
        raise ParseError(f"cannot stringify predicate AST {ast!r}")
    kind = ast[0]
    if kind == "true":
        return "TRUE()"
    if kind == "false":
        return "FALSE()"
    if kind in ("call", "macro"):
        _, name, args = ast
        return f"{name}(" + ", ".join(_arg_to_predicate_source(a) for a in args) + ")"
    if kind == "op":
        _, name, args = ast
        return f"OP_{name}(" + ", ".join(_arg_to_predicate_source(a) for a in args) + ")"
    if kind == "cmp":
        _, op, lhs, rhs = ast
        cmp_text = {
            "CMP_EQ": "EQ",
            "CMP_NE": "NE",
            "CMP_LT": "LT",
            "CMP_LE": "LE",
            "CMP_GT": "GT",
            "CMP_GE": "GE",
        }[op]
        return f"{_ast_to_predicate_source(lhs, 3)} {cmp_text} {_ast_to_predicate_source(rhs, 3)}"
    if kind == "not":
        text = "NOT " + _ast_to_predicate_source(ast[1], 3)
        return f"({text})" if parent_prec > 3 else text
    if kind == "and":
        text = " AND ".join(_ast_to_predicate_source(c, 2) for c in ast[1])
        return f"({text})" if parent_prec > 2 else text
    if kind == "or":
        text = " OR ".join(_ast_to_predicate_source(c, 1) for c in ast[1])
        return f"({text})" if parent_prec > 1 else text
    if kind in ("ident", "int", "list"):
        return _arg_to_predicate_source(ast)
    raise ParseError(f"cannot stringify predicate AST kind {kind!r}")


def _strip_can_kill_boss_conjunct_source(src: str, dungeon: str, context: str) -> str:
    ast = parse_predicate(src)
    occurrences = _count_can_kill_boss_calls(ast, dungeon)
    if occurrences != 1:
        raise ParseError(
            f"{context}: expected exactly one CanKillBoss({dungeon}) conjunct, "
            f"found {occurrences}")
    children = _flatten_and(ast)
    direct = [_is_can_kill_boss_call(c, dungeon) for c in children]
    if sum(1 for is_match in direct if is_match) != 1:
        raise ParseError(
            f"{context}: CanKillBoss({dungeon}) must be a top-level AND conjunct")
    kept = [c for c, is_match in zip(children, direct) if not is_match]
    if not kept:
        return "TRUE()"
    if len(kept) == 1:
        return _ast_to_predicate_source(simplify(kept[0]))
    return _ast_to_predicate_source(simplify(("and", kept)))


def _world_state_switch_predicate(default_predicate: str,
                                  ws_predicates: dict[int, str]) -> str:
    cases = []
    guards = []
    for ws_id, predicate in sorted(ws_predicates.items()):
        if predicate == default_predicate:
            continue
        if ws_id not in _WORLD_STATE_NAME_BY_ID:
            raise ParseError(f"unknown world_state id {ws_id}")
        guard = f"WORLDSTATE_EQ({_WORLD_STATE_NAME_BY_ID[ws_id]})"
        guards.append(guard)
        cases.append(f"({guard} AND ({predicate}))")
    if not cases:
        return default_predicate
    default_guard = guards[0] if len(guards) == 1 else "(" + " OR ".join(guards) + ")"
    cases.append(f"(NOT {default_guard} AND ({default_predicate}))")
    return " OR ".join(cases)


def _door_inert_edge_predicate(dungeon: str, approach_predicate: str) -> str:
    """Preserve door-shuffle identity for factored boss-room edges.

    Door-controlled locations are wrapped later so active door shuffle consults
    the door oracle instead of the vanilla location predicate. Boss-room region
    edges must mirror that split: vanilla approach while doors are inactive,
    open edge while that dungeon's door layout is active.
    """
    door_active = f"OP_DOORS_ACTIVE({dungeon})"
    return f"(NOT {door_active} AND ({approach_predicate})) OR {door_active}"


def _apply_chain_boss_room_factoring(regions, edges, loc_preds, world_state_overrides):
    """Add inert boss-room regions for dungeon-chains and factor Boss predicates.

    The transform preserves off-axis logic: boss_approach(D) moves to a single
    inbound edge, while Boss/Prize locations keep only CanKillBoss(D). If a
    world-state override changes the approach, the edge predicate switches on
    WORLDSTATE_EQ so the base edge remains correct for every active world state.
    """
    for dungeon, boss_loc, prize_loc in CHAIN_BOSS_ROOM_FACTORS:
        boss_def = loc_preds.get(boss_loc)
        prize_def = loc_preds.get(prize_loc)
        if boss_def is None or prize_def is None:
            raise RuntimeError(
                f"dungeon-chain boss factoring missing {boss_loc!r} or {prize_loc!r}")
        parent_region = boss_def.region
        if not parent_region or parent_region not in regions:
            raise RuntimeError(
                f"{boss_loc}: cannot derive boss-room parent from region "
                f"{parent_region!r}")
        if prize_def.region != parent_region:
            raise RuntimeError(
                f"{prize_loc}: prize region {prize_def.region!r} differs from "
                f"{boss_loc} region {parent_region!r}")

        boss_region = f"{dungeon}_BossRoom"
        if boss_region in regions:
            raise RuntimeError(f"derived boss-room region {boss_region!r} already exists")

        base_approach = _strip_can_kill_boss_conjunct_source(
            boss_def.can_reach, dungeon, f"{boss_loc}.can_reach")
        prize_approach = _strip_can_kill_boss_conjunct_source(
            prize_def.can_reach, dungeon, f"{prize_loc}.can_reach")
        if prize_approach != base_approach:
            raise RuntimeError(
                f"{prize_loc}: stripped approach {prize_approach!r} differs from "
                f"{boss_loc} approach {base_approach!r}")

        ws_approaches: dict[int, str] = {}
        for loc_name in (boss_loc, prize_loc):
            for ws_id, override in world_state_overrides.get(loc_name, {}).items():
                approach = _strip_can_kill_boss_conjunct_source(
                    override.can_reach, dungeon, f"ws={ws_id} {loc_name}.can_reach")
                prior = ws_approaches.get(ws_id)
                if prior is not None and prior != approach:
                    raise RuntimeError(
                        f"ws={ws_id} {loc_name}: stripped approach {approach!r} "
                        f"differs from sibling approach {prior!r}")
                ws_approaches[ws_id] = approach

        regions[boss_region] = RegionDef(
            id=boss_region,
            name=f"{regions[parent_region].name} - Boss Room",
            dungeon=dungeon,
            parent=parent_region,
            source="dungeon-chains generated boss-room factor",
        )
        # The edge carries CanKillBoss(D) as well as the approach (fresh-eyes
        # review): under dungeon_chains the successor dungeon's entry edge is
        # REDIRECTED through this region, and the runtime seam only opens once
        # the boss prize is checked — without the kill term the placer could
        # certify the successor while the boss is unkillable (e.g. its required
        # weapon, or under souls its soul, placed inside the successor →
        # unbeatable seed). Off-chains this is logically inert: the only
        # residents of <D>_BossRoom are the Boss/Prize locations, whose own
        # predicates already require CanKillBoss(D).
        approach_pred = _world_state_switch_predicate(base_approach, ws_approaches)
        edges.append(EdgeDef(
            from_=parent_region,
            to=boss_region,
            predicate=_door_inert_edge_predicate(
                dungeon,
                f"({approach_pred}) AND CanKillBoss({dungeon})",
            ),
            source="dungeon-chains generated boss-room factor",
        ))

        kill_only = f"CanKillBoss({dungeon})"
        for loc_name in (boss_loc, prize_loc):
            loc = loc_preds[loc_name]
            loc.region = boss_region
            loc.can_reach = kill_only
            for override in world_state_overrides.get(loc_name, {}).values():
                override.region = boss_region
                override.can_reach = kill_only


# ---------------------------------------------------------------------------
# Predicate DSL parser
# ---------------------------------------------------------------------------
# Grammar (per logic.schema.yaml `predicate_expression`):
#
#   predicate := or_expr
#   or_expr   := and_expr ( "OR" and_expr )*
#   and_expr  := not_expr ( "AND" not_expr )*
#   not_expr  := "NOT" not_expr | atom
#   atom      := op_call | macro_call | "TRUE()" | "FALSE()" | "(" predicate ")"
#   op_call   := OP_NAME "(" arg_list? ")"
#   macro_call:= MacroName "(" arg_list? ")"
#   arg_list  := arg ("," arg)*
#   arg       := identifier | int_literal | list_literal
#   list_literal := "[" identifier ("," identifier)* "]"


# AST node forms:
#   ("op", op_name, [args])
#   ("macro", name, [args])
#   ("and", [child, ...])
#   ("or", [child, ...])
#   ("not", child)
#   ("true",)
#   ("false",)


TOKEN_PATTERNS = [
    (r"\s+", None),                       # whitespace
    (r"AND\b", "AND"),
    (r"OR\b", "OR"),
    (r"NOT\b", "NOT"),
    (r"TRUE\b", "TRUE_LIT"),
    (r"FALSE\b", "FALSE_LIT"),
    # Comparison ops: compile-time pseudo-ops that resolve to TRUE/FALSE during
    # macro expansion (substituted with literal integer args). See `fold_compares`.
    (r"EQ\b", "CMP_EQ"),
    (r"NE\b", "CMP_NE"),
    (r"LT\b", "CMP_LT"),
    (r"LE\b", "CMP_LE"),
    (r"GT\b", "CMP_GT"),
    (r"GE\b", "CMP_GE"),
    (r"\(", "LPAREN"),
    (r"\)", "RPAREN"),
    (r"\[", "LBRACK"),
    (r"\]", "RBRACK"),
    (r",", "COMMA"),
    (r"\d+", "INT"),
    (r"[A-Za-z_][A-Za-z0-9_]*", "IDENT"),
]


class ParseError(Exception):
    pass


def tokenize(text: str) -> list[tuple[str, str]]:
    pos = 0
    tokens = []
    while pos < len(text):
        for pattern, kind in TOKEN_PATTERNS:
            m = re.match(pattern, text[pos:])
            if m:
                if kind is not None:
                    tokens.append((kind, m.group(0)))
                pos += m.end()
                break
        else:
            raise ParseError(f"unexpected character at {pos}: {text[pos:pos+20]!r}")
    return tokens


class Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0

    def peek(self, offset=0):
        if self.pos + offset >= len(self.tokens):
            return ("EOF", "")
        return self.tokens[self.pos + offset]

    def eat(self, kind=None):
        if self.pos >= len(self.tokens):
            raise ParseError(f"unexpected end of input (wanted {kind})")
        tok = self.tokens[self.pos]
        if kind is not None and tok[0] != kind:
            raise ParseError(f"expected {kind} at token {self.pos}, got {tok}")
        self.pos += 1
        return tok

    def parse(self):
        node = self.or_expr()
        if self.pos != len(self.tokens):
            raise ParseError(f"trailing tokens at {self.pos}: {self.tokens[self.pos:]}")
        return node

    def or_expr(self):
        children = [self.and_expr()]
        while self.peek()[0] == "OR":
            self.eat("OR")
            children.append(self.and_expr())
        if len(children) == 1:
            return children[0]
        return ("or", children)

    def and_expr(self):
        children = [self.not_expr()]
        while self.peek()[0] == "AND":
            self.eat("AND")
            children.append(self.not_expr())
        if len(children) == 1:
            return children[0]
        return ("and", children)

    def not_expr(self):
        if self.peek()[0] == "NOT":
            self.eat("NOT")
            child = self.not_expr()
            return ("not", child)
        return self.comparison()

    def comparison(self):
        left = self.atom()
        kind = self.peek()[0]
        if kind in {"CMP_EQ", "CMP_NE", "CMP_LT", "CMP_LE", "CMP_GT", "CMP_GE"}:
            self.eat(kind)
            right = self.atom()  # comparison RHS is also an atom (ident/int/expr)
            return ("cmp", kind, left, right)
        return left

    def atom(self):
        tok = self.peek()
        if tok[0] == "LPAREN":
            self.eat("LPAREN")
            node = self.or_expr()
            self.eat("RPAREN")
            return node
        if tok[0] == "TRUE_LIT":
            self.eat("TRUE_LIT")
            self.eat("LPAREN")
            self.eat("RPAREN")
            return ("true",)
        if tok[0] == "FALSE_LIT":
            self.eat("FALSE_LIT")
            self.eat("LPAREN")
            self.eat("RPAREN")
            return ("false",)
        if tok[0] == "INT":
            return ("int", int(self.eat("INT")[1]))
        if tok[0] == "IDENT":
            name = self.eat("IDENT")[1]
            # If followed by LPAREN, this is an op-call or macro-call.
            # Otherwise it's a bare identifier (parameter reference).
            if self.peek()[0] == "LPAREN":
                self.eat("LPAREN")
                args = []
                if self.peek()[0] != "RPAREN":
                    args.append(self.arg())
                    while self.peek()[0] == "COMMA":
                        self.eat("COMMA")
                        args.append(self.arg())
                self.eat("RPAREN")
                if name.startswith("OP_"):
                    bare = name[3:]
                    return ("op", bare, args)
                return ("call", name, args)
            return ("ident", name)
        raise ParseError(f"unexpected token at atom: {tok}")

    def arg(self):
        tok = self.peek()
        if tok[0] == "LBRACK":
            self.eat("LBRACK")
            items = []
            if self.peek()[0] != "RBRACK":
                items.append(self.eat()[1])
                while self.peek()[0] == "COMMA":
                    self.eat("COMMA")
                    items.append(self.eat()[1])
            self.eat("RBRACK")
            return ("list", items)
        if tok[0] == "INT":
            return ("int", int(self.eat("INT")[1]))
        if tok[0] == "IDENT":
            return ("ident", self.eat("IDENT")[1])
        raise ParseError(f"unexpected token in arg: {tok}")


def parse_predicate(text: str):
    return Parser(tokenize(text)).parse()


# ---------------------------------------------------------------------------
# Op-name resolution
# ---------------------------------------------------------------------------
# Predicate "call" nodes are either ops or macros. We resolve in a second pass
# once we know the op registry and macro set.


def resolve_calls(ast, ops: dict[str, OpDef], macros: dict[str, MacroDef]):
    """Convert ("call", name, args) -> ("op", name, args) or ("macro", name, args)."""
    if not isinstance(ast, tuple):
        return ast
    if ast[0] == "call":
        _, name, args = ast
        new_args = [resolve_calls(a, ops, macros) if isinstance(a, tuple) and a[0] in ("call", "op", "macro", "and", "or", "not", "true", "false", "cmp") else a for a in args]
        if name in ops:
            return ("op", name, new_args)
        if name in macros:
            return ("macro", name, new_args)
        # Treat unknown UPPER_SNAKE as op for well-formedness errors downstream.
        if name.isupper() or "_" in name:
            return ("op", name, new_args)
        return ("macro", name, new_args)
    if ast[0] in ("and", "or"):
        return (ast[0], [resolve_calls(c, ops, macros) for c in ast[1]])
    if ast[0] == "not":
        return ("not", resolve_calls(ast[1], ops, macros))
    if ast[0] == "cmp":
        return ("cmp", ast[1], resolve_calls(ast[2], ops, macros), resolve_calls(ast[3], ops, macros))
    if ast[0] == "op":
        return ("op", ast[1], [resolve_calls(a, ops, macros) if isinstance(a, tuple) and a[0] in ("call",) else a for a in ast[2]])
    return ast


# ---------------------------------------------------------------------------
# Macro expansion
# ---------------------------------------------------------------------------
def expand_macros(ast, macros: dict[str, MacroDef], parsed_macro_bodies: dict[str, object], ops: dict[str, OpDef], depth: int = 0, expanding: set = None):
    """Recursively expand macro calls. Detects cycles."""
    if expanding is None:
        expanding = set()
    if depth > 32:
        raise ParseError(f"macro expansion exceeded depth 32 (cycle?)")
    if not isinstance(ast, tuple):
        return ast
    kind = ast[0]
    if kind == "macro":
        _, name, args = ast
        if name not in parsed_macro_bodies:
            raise ParseError(f"unknown macro {name!r}")
        if name in expanding:
            raise ParseError(f"macro cycle: {expanding} -> {name}")
        macro = macros[name]
        body_ast = parsed_macro_bodies[name]
        if len(args) != len(macro.parameters):
            # Allow arity mismatch only if macro is parameterless and call passes nothing.
            if not (len(macro.parameters) == 0 and len(args) == 0):
                raise ParseError(f"macro {name} expects {len(macro.parameters)} arg(s), got {len(args)}")
        # Substitute parameters in body_ast.
        param_map = dict(zip(macro.parameters, args))
        substituted = substitute_params(body_ast, param_map)
        # Fold compile-time comparisons (turn `min_level EQ 1` -> TRUE/FALSE).
        folded = fold_compares(substituted)
        # Recursively expand any inner macros, then simplify.
        expanded = expand_macros(folded, macros, parsed_macro_bodies, ops, depth + 1, expanding | {name})
        return simplify(expanded)
    if kind in ("and", "or"):
        return (kind, [expand_macros(c, macros, parsed_macro_bodies, ops, depth, expanding) for c in ast[1]])
    if kind == "not":
        return ("not", expand_macros(ast[1], macros, parsed_macro_bodies, ops, depth, expanding))
    if kind == "op":
        return ast  # ops don't contain sub-predicates as args; args are item/region IDs etc.
    return ast


def substitute_params(ast, param_map: dict):
    """Replace ident args matching parameter names with their bound values.

    The bound value for a parameter is whatever the caller passed: an ident, an
    int, or a list. We replace IN PLACE within the AST tree.
    """
    if not isinstance(ast, tuple):
        return ast
    kind = ast[0]
    if kind in ("op", "macro"):
        head = ast[0]
        name = ast[1]
        new_args = []
        for a in ast[2]:
            if isinstance(a, tuple) and a[0] == "ident" and a[1] in param_map:
                # Replace ident with the bound value.
                new_args.append(param_map[a[1]])
            else:
                new_args.append(a)
        return (head, name, new_args)
    if kind in ("and", "or"):
        return (kind, [substitute_params(c, param_map) for c in ast[1]])
    if kind == "not":
        return ("not", substitute_params(ast[1], param_map))
    if kind == "cmp":
        _, op, lhs, rhs = ast
        new_lhs = param_map[lhs[1]] if isinstance(lhs, tuple) and lhs[0] == "ident" and lhs[1] in param_map else substitute_params(lhs, param_map)
        new_rhs = param_map[rhs[1]] if isinstance(rhs, tuple) and rhs[0] == "ident" and rhs[1] in param_map else substitute_params(rhs, param_map)
        return ("cmp", op, new_lhs, new_rhs)
    if kind == "ident":
        if ast[1] in param_map:
            return param_map[ast[1]]
    return ast


def fold_compares(ast, errors: list = None):
    """Evaluate ("cmp", op, lhs, rhs) when both sides are integer literals; replace with TRUE/FALSE.

    If a side is still an unbound ident, emit a warning and treat the comparison
    as TRUE (so the surrounding macro-body branch isn't accidentally lost). This
    matches the agent-authored macro convention where ungrounded parameters
    indicate "Phase A doesn't use this branch — assume true and let later passes
    drop the dead code."
    """
    if errors is None:
        errors = []
    if not isinstance(ast, tuple):
        return ast
    kind = ast[0]
    if kind == "cmp":
        _, op, lhs, rhs = ast
        lhs = fold_compares(lhs, errors)
        rhs = fold_compares(rhs, errors)
        if isinstance(lhs, tuple) and lhs[0] == "int" and isinstance(rhs, tuple) and rhs[0] == "int":
            l, r = lhs[1], rhs[1]
            result = {
                "CMP_EQ": l == r,
                "CMP_NE": l != r,
                "CMP_LT": l < r,
                "CMP_LE": l <= r,
                "CMP_GT": l > r,
                "CMP_GE": l >= r,
            }[op]
            return ("true",) if result else ("false",)
        # Unbound ident on at least one side — a macro-authoring error (a
        # parameter was never grounded by the caller). Defaulting to TRUE is the
        # DANGEROUS direction: it silently collapses both arms of a level/count
        # comparison and grants the stricter logic (e.g. silver-arrow capability)
        # for free, which could certify an unbeatable seed as beatable. Raise a
        # hard error (collected into all_errors) instead of the old silent
        # over-permissive TRUE — so --strict (CI) FAILS the build; a non-strict
        # local build at least degrades to a WARN + FALSE arm.
        # DORMANT today: every call site passes a literal shorthand
        # (CanShootArrowsL1 / CanShootSilvers / HasSwordN), so this never fires.
        raise ParseError(
            f"unbound comparison: {op} {lhs!r} {rhs!r} "
            "(a macro parameter was not grounded by the caller)")
    if kind in ("and", "or"):
        return (kind, [fold_compares(c, errors) for c in ast[1]])
    if kind == "not":
        return ("not", fold_compares(ast[1], errors))
    if kind == "op":
        return ast
    return ast


def simplify(ast):
    """Constant-fold TRUE/FALSE through AND/OR/NOT to drop dead branches.

    Used after macro expansion + compare folding to clean up macros like
    `(branch_A AND TRUE) OR (branch_B AND FALSE)` -> `branch_A`.
    """
    if not isinstance(ast, tuple):
        return ast
    kind = ast[0]
    if kind == "and":
        children = [simplify(c) for c in ast[1]]
        # If any FALSE, whole AND is FALSE.
        if any(c == ("false",) for c in children):
            return ("false",)
        # Drop TRUE children.
        children = [c for c in children if c != ("true",)]
        if not children:
            return ("true",)
        if len(children) == 1:
            return children[0]
        return ("and", children)
    if kind == "or":
        children = [simplify(c) for c in ast[1]]
        if any(c == ("true",) for c in children):
            return ("true",)
        children = [c for c in children if c != ("false",)]
        if not children:
            return ("false",)
        if len(children) == 1:
            return children[0]
        return ("or", children)
    if kind == "not":
        child = simplify(ast[1])
        if child == ("true",):
            return ("false",)
        if child == ("false",):
            return ("true",)
        return ("not", child)
    return ast


# ---------------------------------------------------------------------------
# Bytecode encoder
# ---------------------------------------------------------------------------
def encode_predicate(ast, ops: dict[str, OpDef], items: dict[str, ItemDef], regions: dict[str, RegionDef], locations: dict[str, LocationDef]) -> bytes:
    """Encode a fully macro-expanded AST to the bytecode stream."""
    out = bytearray()
    _emit(ast, out, ops, items, regions, locations)
    return bytes(out)


WORLD_STATES = {"open": 0, "standard": 1, "inverted": 2, "retro": 3}
GOALS = {"ganon": 0, "fast_ganon": 1, "dungeons": 2, "pedestal": 3, "triforce_hunt": 4, "ganonhunt": 5, "completionist": 6}

# ---------------------------------------------------------------------------
# Boss-shuffle runtime (OP_CAN_KILL_BOSS) dispatch tables.
#
# BOSS_KILL_BODIES is indexed by boss-pool index — this order is the CONTRACT
# shared with src/rando/shuffle_boss.c (the kBoss_* enum) and the runtime boss
# assignment that BossShuffle_ComputeAssignment produces. Each body reuses the
# canonical per-dungeon CanKill<Boss> macro, so OP_CAN_KILL_BOSS(<Dungeon>) with
# boss_shuffle OFF compiles to the SAME reachability as the inline macro it
# replaces (default placement byte-identical). `world` is a free ident the
# CanKill* macros accept for arity parity but never read (CanExtendMagic /
# CanMeltThings ignore it) — identical to how the location predicates pass it.
BOSS_KILL_BODIES = [
    "CanKillArmosKnights()",     # 0  kBoss_ArmosKnights
    "CanKillLanmolas(world)",    # 1  kBoss_Lanmolas
    "CanKillMoldorm()",          # 2  kBoss_Moldorm
    "CanKillAgahnim2()",         # 3  kBoss_Agahnim   (pinned; Aga1≈Aga2 kill rule)
    "CanKillHelmasaurKing(world)",  # 4  kBoss_HelmasaurKing
    "CanKillArrghus(world)",     # 5  kBoss_Arrghus
    "CanKillMothula(world)",     # 6  kBoss_Mothula
    "CanKillBlind(world)",       # 7  kBoss_Blind
    "CanKillKholdstare(world)",  # 8  kBoss_Kholdstare
    "CanKillVitreous(world)",    # 9  kBoss_Vitreous
    "CanKillTrinexx(world)",     # 10 kBoss_Trinexx
    "CanKillAgahnim2()",         # 11 kBoss_Agahnim2  (pinned)
]

# dungeon-id (HCE=0..GT=12) → vanilla boss-pool index. MIRRORS shuffle_boss.c
# kBossVanilla; 0xFF = no boss (HCE) / unused. Used by OP_CAN_KILL_BOSS when no
# per-seed boss assignment is installed (NULL context). Logic_SelfCheck
# cross-checks this against shuffle_boss.c's vanilla map at runtime.
DUNGEON_VANILLA_BOSS = [
    0xFF,  # 0  HyruleCastleEscape (no boss)
    0,     # 1  EasternPalace      → ArmosKnights
    1,     # 2  DesertPalace       → Lanmolas
    2,     # 3  TowerOfHera        → Moldorm
    3,     # 4  HyruleCastleTower  → Agahnim   (pinned)
    4,     # 5  PalaceOfDarkness   → HelmasaurKing
    5,     # 6  SwampPalace        → Arrghus
    6,     # 7  SkullWoods         → Mothula
    7,     # 8  ThievesTown        → Blind
    8,     # 9  IcePalace          → Kholdstare
    9,     # 10 MiseryMire         → Vitreous
    10,    # 11 TurtleRock         → Trinexx
    11,    # 12 GanonsTower        → Agahnim2  (pinned)
]
# Phase B Slice 4 §56 — operand lookup tables for the three Phase B ops.
# Difficulty enum mirrors `ItemPoolDifficulty` in `src/rando/rando_settings.h`.
# Glitch levels mirror the `logic` axis: NoGlitches=0, OverworldGlitches=1,
# MajorGlitches=2, HybridMG=3, NoLogic=4.
DIFFICULTY_LEVELS = {"easy": 0, "normal": 1, "hard": 2, "expert": 3}
# Phase B swordless — OP_MODEWEAPONS_EQ operand. Mirrors `ModeWeapons` in
# src/rando/rando_settings.h. `swordless` is the only Phase B-exposed value
# beyond randomized/assured, but all four map for completeness.
MODE_WEAPONS = {"randomized": 0, "assured": 1, "vanilla": 2, "swordless": 3}
GLITCH_LEVELS = {
    "no_glitches": 0,
    "overworld_glitches": 1,
    "major_glitches": 2,
    "hybrid_major_glitches": 3,
    "no_logic": 4,
}
# add-enemy-souls - OP_SOULS_TIER_AT_LEAST operand. Mirrors `SoulsShuffle` in
# src/rando/rando_settings.h.
SOULS_TIERS = {"bosses": 1, "bosses_enemies": 2}
# Populated by load_tricks(op_registry) at codegen startup; keys are trick
# ids (e.g. "boots-clip"), values are bit positions (0..7).
TRICK_BITS: dict[str, int] = {}

# §12.6 — ROM-version verification status. The string→int encoding MUST match
# the `RandoRomVerStatus` enum in src/rando/rando_logic.h.
ROM_VER_STATUS = {
    "untested-on-us10": 0,
    "verified-us10": 1,
    "cross-version": 2,
    "jp10-only": 3,
    "us10-different": 4,
}
# Populated by load_trick_status(op_registry) at codegen startup; keys are
# trick ids in BOTH kebab (`boots-clip`) and snake (`boots_clip`) form (snake is
# what OP_TRICK predicate bodies use), values are the rom_version_status string.
# Used by well_formedness to reject jp10-only tricks in predicate bodies (§12.6.5).
TRICK_STATUS: dict[str, str] = {}


def load_trick_status(path: Path) -> tuple[list[dict], dict[str, str]]:
    """Parse `tricks:` rom_version_status (§12.6.1).

    Returns (rows, id_to_status):
      rows = [{bit, id, status_int}] sorted by bit — for emitting kRandoTrickStatus[].
      id_to_status = {kebab_id: status_str, snake_id: status_str} — for
        well_formedness jp10-only rejection (predicate bodies use the snake id).
    """
    doc = load_yaml(path)
    rows: list[dict] = []
    id_to_status: dict[str, str] = {}
    for raw in doc.get("tricks", []):
        bit = raw["bit"]
        canonical = raw["id"]
        status = raw.get("rom_version_status", "untested-on-us10")
        if status not in ROM_VER_STATUS:
            raise ParseError(
                f"trick {canonical!r}: rom_version_status {status!r} not in "
                f"{sorted(ROM_VER_STATUS)}")
        rows.append({"bit": bit, "id": canonical, "status_int": ROM_VER_STATUS[status]})
        id_to_status[canonical] = status
        snake = canonical.replace("-", "_")
        if snake != canonical:
            id_to_status[snake] = status
    rows.sort(key=lambda r: r["bit"])
    return rows, id_to_status


def load_glitch_levels(path: Path) -> list[dict]:
    """Parse the `glitch_levels:` table (§12.6.2).

    Returns [{level, id, status_int}] sorted by level — for emitting
    kRandoGlitchLevelStatus[]. `level` is the settings.logic value.
    """
    doc = load_yaml(path)
    rows: list[dict] = []
    for raw in doc.get("glitch_levels", []):
        level = raw["level"]
        gid = raw["id"]
        if gid in GLITCH_LEVELS and GLITCH_LEVELS[gid] != level:
            raise ParseError(
                f"glitch_level {gid!r}: level {level} disagrees with the "
                f"codegen GLITCH_LEVELS map ({GLITCH_LEVELS[gid]})")
        status = raw.get("rom_version_status", "untested-on-us10")
        if status not in ROM_VER_STATUS:
            raise ParseError(
                f"glitch_level {gid!r}: rom_version_status {status!r} not in "
                f"{sorted(ROM_VER_STATUS)}")
        rows.append({"level": level, "id": gid, "status_int": ROM_VER_STATUS[status]})
    rows.sort(key=lambda r: r["level"])
    return rows


def load_tricks(path: Path) -> dict[str, int]:
    """Parse the `tricks:` table in op_registry.yaml — map id → bit position.

    Phase B §56: the codegen emits the bit position as the OP_TRICK operand;
    runtime tests `(settings.tricks & (1 << bit)) != 0`. Trick widths cap at
    8 (uint8 settings field); per the registry, only bits 0-7 are valid.

    The registry ids use kebab-case (`boots-clip`) to match the user-facing
    `--settings=tricks=...` CSV grammar. The DSL tokenizer accepts only
    `[A-Za-z_][A-Za-z0-9_]*` (no hyphens), so we ALSO register the
    snake-case variant (`boots_clip`) at the same bit. Predicate authors
    use the snake form in `OP_TRICK(name)`; settings CSV stays kebab.
    """
    doc = load_yaml(path)
    out: dict[str, int] = {}
    for raw in doc.get("tricks", []):
        bit = raw["bit"]
        if not (0 <= bit < 8):
            raise ParseError(f"trick {raw.get('id')!r}: bit {bit} out of range 0..7")
        canonical = raw["id"]
        out[canonical] = bit
        snake = canonical.replace("-", "_")
        if snake != canonical:
            out[snake] = bit
    return out


def _emit(ast, out: bytearray, ops, items, regions, locations):
    kind = ast[0]
    if kind == "true":
        # Vacuous AND (count=0) is true.
        out.append(ops["AND"].id)
        out.append(0)
        return
    if kind == "false":
        # Vacuous OR (count=0) is false.
        out.append(ops["OR"].id)
        out.append(0)
        return
    if kind == "and":
        out.append(ops["AND"].id)
        out.append(len(ast[1]))
        for c in ast[1]:
            _emit(c, out, ops, items, regions, locations)
        return
    if kind == "or":
        out.append(ops["OR"].id)
        out.append(len(ast[1]))
        for c in ast[1]:
            _emit(c, out, ops, items, regions, locations)
        return
    if kind == "not":
        out.append(ops["NOT"].id)
        _emit(ast[1], out, ops, items, regions, locations)
        return
    if kind == "op":
        _, name, args = ast
        if name not in ops:
            raise ParseError(f"unknown op {name!r}")
        op = ops[name]
        out.append(op.id)
        _emit_operands(name, args, out, items, regions)
        return
    raise ParseError(f"cannot encode AST kind {kind!r}")


def _emit_u16le(out: bytearray, v: int):
    out += struct.pack("<H", v)


def _emit_operands(op_name: str, args, out: bytearray, items, regions):
    if op_name == "HAS_ITEM":
        out += struct.pack("<H", _resolve_item(args[0], items))
    elif op_name == "HAS_AMOUNT":
        out += struct.pack("<H", _resolve_item(args[0], items))
        out.append(_resolve_int(args[1]))
    elif op_name == "HAS_ANY_OF":
        lst = _resolve_list(args[0])
        out.append(len(lst))
        for it in lst:
            out += struct.pack("<H", _resolve_item(("ident", it), items))
    elif op_name == "HAS_ANY_COUNT":
        lst = _resolve_list(args[0])
        out.append(len(lst))
        for it in lst:
            out += struct.pack("<H", _resolve_item(("ident", it), items))
        out.append(_resolve_int(args[1]))
    elif op_name == "WORLDSTATE_EQ":
        out.append(WORLD_STATES[_resolve_ident(args[0])])
    elif op_name == "GOAL_EQ":
        out.append(GOALS[_resolve_ident(args[0])])
    elif op_name == "GOAL_REQUIRES_DUNGEON":
        out.append(_resolve_dungeon_id(_resolve_ident(args[0])))
    elif op_name == "DUNGEON_CLEARED":
        out.append(_resolve_dungeon_id(_resolve_ident(args[0])))
    elif op_name == "REGION_REACHABLE":
        out += struct.pack("<H", _resolve_region(args[0], regions))
    elif op_name == "HAS_PRIZE":
        out.append(_resolve_prize_id(_resolve_ident(args[0])))
    elif op_name == "MEDALLION_OPENS":
        out.append(_resolve_entrance_id(_resolve_ident(args[0])))
    elif op_name == "CAN_KILL_BOSS":
        # operand = dungeon id (HCE=0..GT=12); the runtime resolves the boss
        # currently assigned to that dungeon and evaluates its kill predicate.
        out.append(_resolve_dungeon_id(_resolve_ident(args[0])))
    elif op_name == "DOORS_ACTIVE":
        # operand = kDoorTblDungeons index. Its order mirrors dungeon_ids.h for
        # the dungeons used here (HCE/HC=0, EP=1, ..., GT=12).
        out.append(_resolve_dungeon_id(_resolve_ident(args[0])))
    elif op_name == "DOORS_LOC_REACHABLE":
        _emit_u16le(out, _resolve_int(args[0]))
    elif op_name == "ITEM_IS":
        out += struct.pack("<H", _resolve_item(args[0], items))
    elif op_name == "TRICK":
        # Phase B §56: resolve named trick → bit position via TRICK_BITS table
        # (populated by load_tricks at codegen startup).
        name = _resolve_ident(args[0])
        if name not in TRICK_BITS:
            raise ParseError(
                f"OP_TRICK references unknown trick {name!r}; "
                f"known: {sorted(TRICK_BITS.keys())}")
        out.append(TRICK_BITS[name])
    elif op_name == "DIFFICULTY_AT_LEAST":
        name = _resolve_ident(args[0])
        if name not in DIFFICULTY_LEVELS:
            raise ParseError(
                f"OP_DIFFICULTY_AT_LEAST: unknown difficulty {name!r}; "
                f"known: {sorted(DIFFICULTY_LEVELS.keys())}")
        out.append(DIFFICULTY_LEVELS[name])
    elif op_name == "GLITCH_LEVEL_AT_LEAST":
        name = _resolve_ident(args[0])
        if name not in GLITCH_LEVELS:
            raise ParseError(
                f"OP_GLITCH_LEVEL_AT_LEAST: unknown glitch level {name!r}; "
                f"known: {sorted(GLITCH_LEVELS.keys())}")
        out.append(GLITCH_LEVELS[name])
    elif op_name == "SOULS_TIER_AT_LEAST":
        name = _resolve_ident(args[0])
        if name not in SOULS_TIERS:
            raise ParseError(
                f"OP_SOULS_TIER_AT_LEAST: unknown souls tier {name!r}; "
                f"known: {sorted(SOULS_TIERS.keys())}")
        out.append(SOULS_TIERS[name])
    elif op_name == "MODEWEAPONS_EQ":
        name = _resolve_ident(args[0])
        if name not in MODE_WEAPONS:
            raise ParseError(
                f"OP_MODEWEAPONS_EQ: unknown mode.weapons {name!r}; "
                f"known: {sorted(MODE_WEAPONS.keys())}")
        out.append(MODE_WEAPONS[name])
    elif op_name == "NPC_SOULS_ACTIVE":
        if args:
            raise ParseError("OP_NPC_SOULS_ACTIVE takes no operands")
    elif op_name == "TOWER_CRYSTALS_MET":
        if args:
            raise ParseError("OP_TOWER_CRYSTALS_MET takes no operands")
    elif op_name == "OW_FLUTE_VANILLA":
        if args:
            raise ParseError("OP_OW_FLUTE_VANILLA takes no operands")
    elif op_name == "INSTANT_FLUTE":
        if args:
            raise ParseError("OP_INSTANT_FLUTE takes no operands")
    elif op_name == "POT_KEYS_ON":
        if args:
            raise ParseError("OP_POT_KEYS_ON takes no operands")
    elif op_name == "POT_KEYS_WILD":
        if args:
            raise ParseError("OP_POT_KEYS_WILD takes no operands")
    elif op_name == "POT_KEYS_DUNGEON":
        if args:
            raise ParseError("OP_POT_KEYS_DUNGEON takes no operands")
    elif op_name == "ENEMY_DROP_KEYS_WILD":
        if args:
            raise ParseError("OP_ENEMY_DROP_KEYS_WILD takes no operands")
    elif op_name == "ENEMY_DROP_KEYS_DUNGEON":
        if args:
            raise ParseError("OP_ENEMY_DROP_KEYS_DUNGEON takes no operands")
    else:
        raise ParseError(f"no operand-emit rule for op {op_name!r}")


def _resolve_item(arg, items):
    if isinstance(arg, tuple) and arg[0] == "ident":
        nm = arg[1]
    elif isinstance(arg, str):
        nm = arg
    else:
        raise ParseError(f"item arg expected ident, got {arg!r}")
    if nm not in items:
        raise ParseError(f"unknown item ID {nm!r}")
    return items[nm].id


def _resolve_int(arg):
    if isinstance(arg, tuple) and arg[0] == "int":
        return arg[1]
    if isinstance(arg, int):
        return arg
    raise ParseError(f"int arg expected, got {arg!r}")


def _resolve_list(arg):
    if isinstance(arg, tuple) and arg[0] == "list":
        return arg[1]
    raise ParseError(f"list arg expected, got {arg!r}")


def _resolve_ident(arg):
    if isinstance(arg, tuple) and arg[0] == "ident":
        return arg[1]
    if isinstance(arg, str):
        return arg
    raise ParseError(f"ident arg expected, got {arg!r}")


def _resolve_region(arg, regions):
    name = _resolve_ident(arg)
    if name in regions:
        # Stable IDs assigned by the codegen region order.
        return _region_ids_for_codegen(regions).index(name)
    # Unknown region name — almost always a translation typo (e.g.
    # "DesertPalace" instead of "DesertPalace_Lobby"). The old behavior emitted a
    # _stable_hash16 fallback that is essentially never a valid region index, so
    # the predicate's region term was PERMANENTLY FALSE at runtime — silently
    # stranding a location/edge, surfacing only in playtest. The "--strict
    # promotes to fail" claim was itself stale: the WARN was a bare stderr print,
    # never collected into all_errors, so --strict did NOT catch it. Raise a hard
    # parse error (now collected into all_errors) so --strict (CI) FAILS on a
    # typoed region; a non-strict local build degrades to a WARN + FALSE term —
    # still better than the old silent never-valid hash. The committed
    # predicates all resolve, so this never fires on an unmodified tree.
    raise ParseError(
        f"OP_REGION_REACHABLE references unknown region {name!r}; "
        f"did you mean one of: {sorted(regions.keys())[:6]}"
        f"{'...' if len(regions) > 6 else ''}?")


def _resolve_dungeon_id(name: str) -> int:
    dungeons = ["HyruleCastleEscape", "EasternPalace", "DesertPalace", "TowerOfHera",
                "HyruleCastleTower", "PalaceOfDarkness", "SwampPalace", "SkullWoods",
                "ThievesTown", "IcePalace", "MiseryMire", "TurtleRock", "GanonsTower"]
    if name not in dungeons:
        raise ParseError(f"unknown dungeon {name!r}; valid: {dungeons}")
    return dungeons.index(name)


def _resolve_prize_id(name: str) -> int:
    prizes = ["Prize_GreenPendant", "Prize_RedPendant", "Prize_BluePendant",
              "Prize_Crystal1", "Prize_Crystal2", "Prize_Crystal3", "Prize_Crystal4",
              "Prize_Crystal5", "Prize_Crystal6", "Prize_Crystal7"]
    if name not in prizes:
        raise ParseError(f"unknown prize {name!r}")
    return prizes.index(name)


def _resolve_entrance_id(name: str) -> int:
    entrances = ["MiseryMire_Entrance", "TurtleRock_Entrance"]
    if name not in entrances:
        raise ParseError(f"unknown medallion-affected entrance {name!r}; valid: {entrances}")
    return entrances.index(name)


def _stable_hash16(s: str) -> int:
    h = 0
    for ch in s:
        h = (h * 1315423911 + ord(ch)) & 0xFFFF
    return h


# ---------------------------------------------------------------------------
# Well-formedness checks (task 3.10)
# ---------------------------------------------------------------------------
def well_formedness(ast, ops, items, regions, locations, context: str, phase_a_only: bool = False, errors: list = None):
    """Walk AST, check:
    - referenced items / regions / dungeons exist;
    - OP_ITEM_IS only in can_place context;
    - (phase_a_only=True only) no Phase B ops in Phase A predicates.
    Phase B Slice 4 §56 flipped the default to allow trick/difficulty/
    glitch ops; the strict-Phase-A gate remains available for callers
    that explicitly opt in.
    Returns the list of error strings (in-place modifications).
    """
    if errors is None:
        errors = []
    if not isinstance(ast, tuple):
        return errors
    kind = ast[0]
    if kind == "op":
        _, name, args = ast
        if name not in ops:
            errors.append(f"unknown op {name!r}")
            return errors
        op = ops[name]
        if phase_a_only and op.phase == "B":
            errors.append(f"Phase B op {name!r} not allowed in Phase A predicate ({context})")
        if name == "ITEM_IS" and context not in ("can_place", "always_allow"):
            errors.append(f"OP_ITEM_IS only allowed in placement-context predicates (can_place / always_allow), used in {context}")
        # §12.6.5 — a predicate MUST NOT reference a trick whose
        # rom_version_status is jp10-only (confirmed NOT to work on US 1.0).
        if name == "TRICK" and args:
            tname = _resolve_ident(args[0])
            if TRICK_STATUS.get(tname) == "jp10-only":
                errors.append(
                    f"OP_TRICK({tname}) references a jp10-only trick (does not "
                    f"work on US 1.0); remove it from {context} or change its "
                    f"rom_version_status in op_registry.yaml")
        # Validate operand references (best-effort)
        if name in ("HAS_ITEM", "HAS_AMOUNT", "ITEM_IS"):
            it = _resolve_ident(args[0]) if args else None
            if it and it not in items:
                errors.append(f"unknown item {it!r} in {context}")
        if name in ("HAS_ANY_OF", "HAS_ANY_COUNT"):
            if args:
                lst = _resolve_list(args[0]) if isinstance(args[0], tuple) and args[0][0] == "list" else []
                for it in lst:
                    if it not in items:
                        errors.append(f"unknown item {it!r} in {context}")
    if kind in ("and", "or"):
        for c in ast[1]:
            well_formedness(c, ops, items, regions, locations, context, phase_a_only, errors)
    if kind == "not":
        well_formedness(ast[1], ops, items, regions, locations, context, phase_a_only, errors)
    return errors


# ---------------------------------------------------------------------------
# Codegen emitters
# ---------------------------------------------------------------------------
HEADER_BANNER = """\
// AUTO-GENERATED by assets/rando_logic_gen.py. DO NOT EDIT.
// Regenerate by running: python assets/rando_logic_gen.py
//
// Source artifacts:
//   - assets/rando/op_registry.yaml
//   - assets/rando/item_registry.yaml
//   - assets/rando/location_registry.yaml
//   - assets/rando/logic.yaml (optional; empty graph if missing)
//   - assets/rando/macros.yaml (optional)
"""


def sanitize_for_define(s: str) -> str:
    """Convert a free-form name to a valid C identifier suffix.

    Replaces non-alphanumeric chars with underscore; collapses runs.
    """
    out = re.sub(r"[^A-Za-z0-9]+", "_", s).strip("_")
    return out


def emit_hint_metadata(metadata: HintMetadata, path: Path) -> None:
    """Emit the private Hints v2 item/location metadata include."""
    lines = [
        HEADER_BANNER,
        "//   - assets/rando/hint_metadata.yaml",
        "//   - assets/rando/hint_metadata.lock.json",
        "//   - assets/rando/hint_registry_contract.json",
        "",
        "#ifndef ZELDA3_RANDO_HINT_METADATA_H_",
        "#define ZELDA3_RANDO_HINT_METADATA_H_",
        "",
        "#include \"../types.h\"",
        "#include \"item_ids.h\"",
        "",
        "#define kGeneratedHintItemFlag_Useful   0x01u",
        "#define kGeneratedHintItemFlag_Major    0x02u",
        "#define kGeneratedHintItemFlag_Priority 0x04u",
        "#define kGeneratedHintItemFlag_Junk      0x08u",
        "",
        "typedef struct GeneratedHintItemMetadata {",
        "  const char *short_name;",
        "  const char *qualified_name;",
        "  const char *compact_qualified_name;",
        "  uint8 flags;",
        "  uint8 diversity_group;  // 0 = this item id is its own family.",
        "} GeneratedHintItemMetadata;",
        "",
        "typedef struct GeneratedHintLocationMetadata {",
        "  const char *compact_name;",
        "  uint16 location_id;",
        "} GeneratedHintLocationMetadata;",
        "",
        "typedef struct GeneratedHintRegionMetadata {",
        "  const char *qualified_name;",
        "  const char *compact_name;",
        "  uint16 region_id;",
        "} GeneratedHintRegionMetadata;",
        "",
        f"#define kGeneratedHintMetadataFormatVersion "
        f"{metadata.format_version}u",
        f"#define kGeneratedHintMetadataAlgorithmVersion "
        f"{metadata.algorithm_version}u",
        f"#define kGeneratedHintMetadataTextSchemaVersion "
        f"{metadata.text_schema_version}u",
        f"#define kGeneratedHintItemMetadataCount {len(metadata.items)}u",
        f"#define kGeneratedHintLocationMetadataCount "
        f"{len(metadata.locations)}u",
        f"#define kGeneratedHintRegionMetadataCount "
        f"{len(metadata.regions)}u",
        f"#define kGeneratedHintHighFrictionLocationCount "
        f"{len(metadata.high_friction_location_ids)}u",
        "",
        "static const char kGeneratedHintMetadataApprovedAlgorithmFingerprint[] =",
        f"    {json.dumps(metadata.algorithm_fingerprint)};",
        "static const char kGeneratedHintMetadataApprovedTextFingerprint[] =",
        f"    {json.dumps(metadata.text_fingerprint)};",
        "",
        "static const GeneratedHintItemMetadata "
        "kGeneratedHintItemMetadata[ITEM__COUNT] = {",
    ]
    for item in metadata.items:
        lines.append(
            "  { %s, %s, %s, 0x%02xu, %uu },  // %u: %s"
            % (
                json.dumps(item.short_name, ensure_ascii=True),
                json.dumps(item.qualified_name, ensure_ascii=True),
                json.dumps(item.compact_qualified_name, ensure_ascii=True),
                item.flags,
                item.diversity_group,
                item.item_id,
                item.registry_name,
            )
        )
    lines.extend(
        [
            "};",
            "",
            "static const GeneratedHintLocationMetadata "
            "kGeneratedHintLocationMetadata[] = {",
        ]
    )
    for location in metadata.locations:
        lines.append(
            "  { %s, %uu },  // %s"
            % (
                json.dumps(location.compact_name, ensure_ascii=True),
                location.location_id,
                location.registry_name,
            )
        )
    lines.extend(
        [
            "};",
            "",
            "static const GeneratedHintRegionMetadata "
            "kGeneratedHintRegionMetadata[] = {",
        ]
    )
    for region in metadata.regions:
        lines.append(
            "  { %s, %s, %uu },  // %s"
            % (
                json.dumps(region.qualified_name, ensure_ascii=True),
                json.dumps(region.compact_name, ensure_ascii=True),
                region.region_id,
                region.registry_name,
            )
        )
    lines.extend(
        [
            "};",
            "",
            "static const uint16 kGeneratedHintHighFrictionLocations[] = {",
        ]
    )
    for location_id in metadata.high_friction_location_ids:
        lines.append(f"  {location_id}u,")
    lines.extend(
        [
            "};",
            "",
            "_Static_assert(kGeneratedHintItemMetadataCount == ITEM__COUNT,",
            '               "hint item metadata must cover every registry item");',
            "",
            "#endif  // ZELDA3_RANDO_HINT_METADATA_H_",
        ]
    )
    atomic_write_text(path, "\n".join(lines) + "\n")


def emit_location_ids(locations: dict[str, LocationDef], path: Path):
    lines = [HEADER_BANNER, "", "#ifndef ZELDA3_RANDO_LOCATION_IDS_H_", "#define ZELDA3_RANDO_LOCATION_IDS_H_", "", "// Stable numeric location IDs from assets/rando/location_registry.yaml.", "// Renaming a location does NOT change its numeric ID (append-only registry).", ""]
    for loc in sorted(locations.values(), key=lambda l: l.id):
        token = sanitize_for_define(loc.name)
        lines.append(f"#define LOC_{token} {loc.id}")
    max_id = max(l.id for l in locations.values()) if locations else 0
    lines.append("")
    lines.append(f"#define LOC__COUNT {max_id + 1}")
    lines.append("")
    # Every location-id-keyed array/bitmap across the randomizer module is sized
    # by kRandoLocationCapacity (rando_logic.h), and the reachability/sphere
    # bitsets in rando_logic.c follow it. A registry append that pushes a
    # location id past that ceiling must fail the BUILD, not silently OOB-index a
    # bitset / truncate the digest / drop a tracker row at runtime. Keep this
    # 8192 in lockstep with kRandoLocationCapacity in src/rando/rando_logic.h.
    lines.append("_Static_assert(LOC__COUNT <= 8192,")
    lines.append('               "location id space exceeds kRandoLocationCapacity '
                 '(8192) in rando_logic.h — grow both together");')
    lines.append("")
    lines.append("#endif  // ZELDA3_RANDO_LOCATION_IDS_H_")
    atomic_write_text(path, "\n".join(lines) + "\n")


def emit_item_ids(items: dict[str, ItemDef], path: Path):
    lines = [HEADER_BANNER, "", "#ifndef ZELDA3_RANDO_ITEM_IDS_H_", "#define ZELDA3_RANDO_ITEM_IDS_H_", "", "// Stable numeric item IDs from assets/rando/item_registry.yaml.", "// Used by Rando_OnLocationCheck, predicate VM, placement table.", ""]
    for it in sorted(items.values(), key=lambda i: i.id):
        token = sanitize_for_define(it.name)
        lines.append(f"#define ITEM_{token} {it.id}")
    max_id = max(i.id for i in items.values()) if items else 0
    lines.append("")
    lines.append(f"#define ITEM__COUNT {max_id + 1}")
    lines.append("")
    lines.append("#endif  // ZELDA3_RANDO_ITEM_IDS_H_")
    atomic_write_text(path, "\n".join(lines) + "\n")


_BOTTLE_CONTENT_RECEIVE_CODES = {
    2: 0x16,  # empty
    3: 0x2B,  # bee
    4: 0x2C,  # fairy
    5: 0x2D,  # red potion
    6: 0x3D,  # green potion
    7: 0x3C,  # good bee
    8: 0x48,  # blue potion
}
_RUPEE_RECEIVE_CODES = {1: 0x34, 5: 0x35, 20: 0x36, 50: 0x41, 100: 0x40, 300: 0x46}
_PROGRESSIVE_GRANT_OPS = {
    "progressive_sword": "kRandoGrantOp_ProgressiveSword",
    "progressive_shield": "kRandoGrantOp_ProgressiveShield",
    "progressive_armor": "kRandoGrantOp_ProgressiveArmor",
    "progressive_glove": "kRandoGrantOp_ProgressiveGlove",
    "progressive_bow": "kRandoGrantOp_ProgressiveBow",
}
_DUNGEON_GRANT_OPS = {
    "dungeon_item_small_key": "kRandoGrantOp_DungeonSmallKey",
    "dungeon_item_big_key": "kRandoGrantOp_DungeonBigKey",
    "dungeon_item_map": "kRandoGrantOp_DungeonMap",
    "dungeon_item_compass": "kRandoGrantOp_DungeonCompass",
}


def _parse_grant_metadata(it: ItemDef) -> tuple[str, int]:
    """Map one registry dispatch token to its semantic C opcode + payload.

    This is deliberately exhaustive. Adding a new spelling to the YAML fails
    codegen until its runtime semantics are chosen here, preventing a placeable
    item from falling through to the source location's vanilla item.
    """
    token = it.dispatch.strip()
    if token.startswith("vanilla:"):
        try:
            code = int(token.split(":", 1)[1], 0)
        except ValueError as e:
            raise RuntimeError(f"item {it.name}: invalid dispatch token {token!r}") from e
        if not 0 <= code <= 0xFF:
            raise RuntimeError(f"item {it.name}: receive code out of range in {token!r}")
        # These ALTTPR item bytes are outside this port's 76-entry vanilla
        # receive table and therefore have semantic direct handlers.
        special = {
            0x51: ("kRandoGrantOp_DirectBombCapacity", 5),
            0x52: ("kRandoGrantOp_DirectArrowCapacity", 5),
            0xAF: ("kRandoGrantOp_DirectGenericKey", it.id),
        }
        if code in special:
            return special[code]
        if code >= 76:
            raise RuntimeError(
                f"item {it.name}: {token!r} exceeds the receive table and has no semantic handler"
            )
        return "kRandoGrantOp_Receive", code

    if token in _PROGRESSIVE_GRANT_OPS:
        return _PROGRESSIVE_GRANT_OPS[token], 0
    if token in _DUNGEON_GRANT_OPS:
        return _DUNGEON_GRANT_OPS[token], it.id
    if token.startswith("bottle_contents:"):
        try:
            contents = int(token.split(":", 1)[1], 0)
            code = _BOTTLE_CONTENT_RECEIVE_CODES[contents]
        except (ValueError, KeyError) as e:
            raise RuntimeError(f"item {it.name}: unsupported dispatch token {token!r}") from e
        return "kRandoGrantOp_Receive", code
    if token.startswith("rupee_tier:"):
        try:
            amount = int(token.split(":", 1)[1], 0)
            code = _RUPEE_RECEIVE_CODES[amount]
        except (ValueError, KeyError) as e:
            raise RuntimeError(f"item {it.name}: unsupported dispatch token {token!r}") from e
        return "kRandoGrantOp_Receive", code
    if token.startswith("prize_pendant:") or token.startswith("prize_crystal:"):
        return "kRandoGrantOp_DirectPrize", it.id
    if token in ("direct_half_magic", "direct_quarter_magic"):
        return "kRandoGrantOp_DirectMagicUpgrade", it.id
    if token == "direct_silver_arrows":
        return "kRandoGrantOp_Receive", 0x3B
    if token == "direct_triforce_piece":
        return "kRandoGrantOp_DirectTriforcePiece", it.id
    if token == "direct_key_ring":
        return "kRandoGrantOp_DirectKeyRing", it.id
    if token == "direct_skeleton_key":
        return "kRandoGrantOp_DirectSkeletonKey", it.id
    if token == "direct_rupoor":
        return "kRandoGrantOp_DirectRupoor", it.id
    if token.startswith("direct_trap_"):
        return "kRandoGrantOp_DirectTrap", it.id
    if token == "direct_nothing":
        return "kRandoGrantOp_DirectNothing", it.id
    if token == "direct_soul":
        return "kRandoGrantOp_DirectSoul", it.id
    if token == "virtual":
        return "kRandoGrantOp_Virtual", it.id
    raise RuntimeError(f"item {it.name}: unknown dispatch token {token!r}")


def emit_item_grant_metadata(items: dict[str, ItemDef], out: list[str]) -> None:
    ordered = sorted(items.values(), key=lambda i: i.id)
    if not ordered:
        raise RuntimeError("item registry is empty")
    expected_ids = list(range(ordered[-1].id + 1))
    actual_ids = [it.id for it in ordered]
    if actual_ids != expected_ids:
        raise RuntimeError(
            "item registry ids must be unique and contiguous for grant metadata: "
            f"expected {expected_ids!r}, got {actual_ids!r}"
        )
    out.extend([
        "// Semantic grant opcode + payload for every authoritative item id.",
        "// Generated from item_registry.yaml dispatch tokens; keep grant/draw planning data-driven.",
        "const RandoItemGrantMetadata kRandoItemGrantMetadata[ITEM__COUNT] = {",
    ])
    for it in ordered:
        opcode, payload = _parse_grant_metadata(it)
        # Round-trip the semantic parse in-process so formatting/refactors cannot
        # accidentally emit a different payload than the validated token.
        if _parse_grant_metadata(it) != (opcode, payload):
            raise RuntimeError(f"item {it.name}: non-deterministic grant metadata parse")
        out.append(
            f"  {{ {opcode}, 0x{payload:04x} }},  // {it.id}: {it.name} ({it.dispatch})"
        )
    out.extend([
        "};",
        "const uint16 kRandoItemGrantMetadataCount = ITEM__COUNT;",
        "",
    ])


def _write_empty_chest_lookup(path: Path) -> None:
    """Emit an empty chest_lookup.h (no assets extracted, e.g. CI).

    1-element sentinel array (never read, _COUNT is 0) for portable C89.
    chest_lookup() bounds its search by _COUNT, so every lookup returns 0xFFFF.
    """
    lines = [
        HEADER_BANNER,
        "",
        "// chest_lookup.h — EMPTY: no vanilla chest table was available at",
        "// codegen time (assets/rando/chest_table.gen.bin absent — no ROM",
        "// extracted, e.g. a CI build). chest_lookup() returns 0xFFFF for every",
        "// (room, ordinal), so all chests fall through to vanilla. Re-run asset",
        "// extraction and rebuild to populate this table for a playable build.",
        "",
        "#ifndef ZELDA3_RANDO_CHEST_LOOKUP_H_",
        "#define ZELDA3_RANDO_CHEST_LOOKUP_H_",
        "",
        "#include \"../types.h\"",
        "#include \"location_ids.h\"",
        "",
        "typedef struct RandoChestLookupEntry {",
        "  uint16 room;     // 0..319 dungeon room index",
        "  uint8  ordinal;  // 0..5; in-room chest index = (tile - 0x58)",
        "  uint16 loc_id;   // LOC_*",
        "} RandoChestLookupEntry;",
        "",
        "// Unused sentinel (kRandoChestLookup_COUNT == 0 — never indexed).",
        "static const RandoChestLookupEntry kRandoChestLookup[1] = { { 0, 0, 0 } };",
        "",
        "#define kRandoChestLookup_COUNT 0",
        "",
        "#endif  // ZELDA3_RANDO_CHEST_LOOKUP_H_",
    ]
    atomic_write_text(path, "\n".join(lines) + "\n")


def emit_pot_lookup(rows, path: Path) -> int:
    """Emit src/rando/pot_lookup.h — sorted (dungeon_room, tile_position) -> LOC.

    The runtime pot dispatch (Dungeon_GetPotLocation) binary-searches this for the
    pot's stable (room, pos4) identity. Mirrors chest_lookup.h. `rows` come from
    load_pots() (pots.gen.yaml); sorted by (room, pos4) so the search is correct."""
    rows = sorted(rows, key=lambda r: (r[0], r[1]))
    digest = _pot_registry_digest(rows)
    lines = [
        HEADER_BANNER, "",
        "// pot_lookup.h — sorted (dungeon_room_index, tile_position) -> LOC_* for",
        "// the runtime pot dispatch (Dungeon_GetPotLocation). Generated from",
        "// local assets/rando/pots.gen.yaml via assets/scripts/gen_pot_tables.py.",
        "",
        "#ifndef ZELDA3_RANDO_POT_LOOKUP_H_",
        "#define ZELDA3_RANDO_POT_LOOKUP_H_",
        "",
        "#include \"../types.h\"",
        "",
        "typedef struct RandoPotLookupEntry {",
        "  uint16 room;    // 0..319 dungeon room index",
        "  uint16 pos4;    // tile_position (dsto*2 | 0x2000 BG-half), == RevealPotItem's match key",
        "  uint16 loc_id;  // LOC_*",
        "} RandoPotLookupEntry;",
        "",
    ]
    if rows:
        lines.append("static const RandoPotLookupEntry kRandoPotLookup[] = {")
        for (room, pos4, locid) in rows:
            lines.append(f"  {{ 0x{room:04x}, 0x{pos4:04x}, {locid} }},")
        lines.append("};")
    else:
        lines.append("// EMPTY: pots.gen.yaml absent. Active pot-shuffle generation fails closed.")
        lines.append("static const RandoPotLookupEntry kRandoPotLookup[1] = { { 0, 0, 0 } };")
    lines += [
        "",
        f"#define kRandoPotLookup_COUNT {len(rows)}",
        f"#define kRandoPotRegistryCount {len(rows)}",
        f"#define kRandoPotRegistryDigest 0x{digest:08x}u",
        "",
        "#endif  // ZELDA3_RANDO_POT_LOOKUP_H_",
    ]
    atomic_write_text(path, "\n".join(lines) + "\n")
    return len(rows)


def emit_terrain_lookup(rows, path: Path) -> int:
    """Emit src/rando/terrain_lookup.h — sorted (screen, pos) -> LOC_* for the
    runtime terrain reveal dispatch (add-rando-grass-rock-shuffle Phase 4's
    Rando_TerrainRevealHook). Mirrors pot_lookup.h; `rows` come from
    load_terrain() (terrain.gen.yaml). The FNV digest doubles as the sidecar
    activation-guard value."""
    rows = sorted(rows, key=lambda r: (r[0], r[1]))
    digest = _terrain_registry_digest(rows)
    lines = [
        HEADER_BANNER, "",
        "// terrain_lookup.h — sorted (overworld_screen_index, map16 pos) -> LOC_*",
        "// for the runtime terrain reveal dispatch (Rando_TerrainRevealHook).",
        "// Generated from local assets/rando/terrain.gen.yaml via",
        "// assets/scripts/gen_terrain_tables.py --emit.",
        "",
        "#ifndef ZELDA3_RANDO_TERRAIN_LOOKUP_H_",
        "#define ZELDA3_RANDO_TERRAIN_LOOKUP_H_",
        "",
        "#include \"../types.h\"",
        "",
        "typedef struct RandoTerrainLookupEntry {",
        "  uint16 screen;  // 0x00..0x7F parent overworld area",
        "  uint16 pos;     // area-relative map16 pos, == Overworld_RevealSecret's key",
        "  uint16 loc_id;  // LOC_*",
        "} RandoTerrainLookupEntry;",
        "",
    ]
    if rows:
        lines.append("static const RandoTerrainLookupEntry kRandoTerrainLookup[] = {")
        for (screen, pos, locid, _ws_mask) in rows:  # ws_mask is digest-only
            lines.append(f"  {{ 0x{screen:02x}, 0x{pos:04x}, {locid} }},")
        lines.append("};")
    else:
        lines.append("// EMPTY: terrain.gen.yaml absent. Active grass/rock generation fails closed.")
        lines.append("static const RandoTerrainLookupEntry kRandoTerrainLookup[1] = { { 0, 0, 0 } };")
    lines += [
        "",
        f"#define kRandoTerrainLookup_COUNT {len(rows)}",
        f"#define kRandoTerrainRegistryCount {len(rows)}",
        f"#define kRandoTerrainRegistryDigest 0x{digest:08x}u",
        "",
        "#endif  // ZELDA3_RANDO_TERRAIN_LOOKUP_H_",
    ]
    atomic_write_text(path, "\n".join(lines) + "\n")
    return len(rows)


def emit_bonk_lookup(rows, path: Path) -> int:
    """Emit src/rando/bonk_lookup.h — sorted (area, block) -> LOC_* for the
    runtime bonk wake dispatch (add-rando-bonk-sanity). Mirrors
    terrain_lookup.h; `rows` come from load_bonk() (bonk.gen.yaml). The FNV
    digest doubles as the sidecar activation-guard value."""
    rows = sorted(rows, key=lambda r: (r[0], r[1]))
    digest = _bonk_registry_digest(rows)
    lines = [
        HEADER_BANNER, "",
        "// bonk_lookup.h — sorted (overworld area, sprite block) -> LOC_* for",
        "// the runtime bonk wake dispatch (Entity_ApplyRumbleToSprites hook).",
        "// `block` is the engine key Overworld_LoadSprites stores sprites",
        "// under (sprite_where_in_overworld[]) and sprite_N_word[k] recovers.",
        "// Generated from local assets/rando/bonk.gen.yaml via",
        "// assets/scripts/gen_bonk_tables.py.",
        "",
        "#ifndef ZELDA3_RANDO_BONK_LOOKUP_H_",
        "#define ZELDA3_RANDO_BONK_LOOKUP_H_",
        "",
        "#include \"../types.h\"",
        "",
        "typedef struct RandoBonkLookupEntry {",
        "  uint16 area;    // 0x00..0x8F overworld area index",
        "  uint16 block;   // sprite_where_in_overworld index (r5|r6<<8 packing)",
        "  uint16 loc_id;  // LOC_*",
        "} RandoBonkLookupEntry;",
        "",
    ]
    if rows:
        lines.append("static const RandoBonkLookupEntry kRandoBonkLookup[] = {")
        for (area, block, locid) in rows:
            lines.append(f"  {{ 0x{area:02x}, 0x{block:04x}, {locid} }},")
        lines.append("};")
    else:
        lines.append("// EMPTY: bonk.gen.yaml absent. Active bonk generation fails closed.")
        lines.append("static const RandoBonkLookupEntry kRandoBonkLookup[1] = { { 0, 0, 0 } };")
    lines += [
        "",
        f"#define kRandoBonkLookup_COUNT {len(rows)}",
        f"#define kRandoBonkRegistryCount {len(rows)}",
        f"#define kRandoBonkRegistryDigest 0x{digest:08x}u",
        "",
        "#endif  // ZELDA3_RANDO_BONK_LOOKUP_H_",
    ]
    atomic_write_text(path, "\n".join(lines) + "\n")
    return len(rows)


def emit_enemy_drop_lookup(rows, items: dict[str, ItemDef], path: Path) -> int:
    """Emit src/rando/enemy_drop_lookup.h for forced enemy-key checks."""
    rows = sorted(rows, key=lambda r: (r["room"], r["source_slot"], r["drop_kind"]))
    hce_big_key_loc = 0xFFFF
    for r in rows:
        if r["drop_kind"] == "big_key":
            if hce_big_key_loc != 0xFFFF:
                raise RuntimeError("enemy_drop_lookup: multiple one-shot big-key rows")
            hce_big_key_loc = int(r["loc_id"])
    lines = [
        HEADER_BANNER, "",
        "// enemy_drop_lookup.h — sorted (dungeon_room_index, source_slot, drop_kind)",
        "// -> LOC_* for forced enemy key-drop checks. Generated from local",
        "// assets/rando/enemy_drops.gen.yaml via assets/scripts/gen_enemy_drop_tables.py.",
        "",
        "#ifndef ZELDA3_RANDO_ENEMY_DROP_LOOKUP_H_",
        "#define ZELDA3_RANDO_ENEMY_DROP_LOOKUP_H_",
        "",
        "#include \"../types.h\"",
        "#include \"item_ids.h\"",
        "",
        "enum {",
        "  kRandoEnemyDropKind_SmallKey = 1,",
        "  kRandoEnemyDropKind_BigKey = 2,",
        "};",
        "",
        "typedef struct RandoEnemyDropLookupEntry {",
        "  uint16 room;             // 0..319 dungeon room index",
        "  uint8  source_slot;      // runtime sprite slot of the carrier source",
        "  uint8  drop_kind;        // kRandoEnemyDropKind_*",
        "  uint16 loc_id;           // LOC_* value",
        "  uint16 vanilla_item_id;  // ITEM_SmallKey_* for Phase 1 rows",
        "} RandoEnemyDropLookupEntry;",
        "",
    ]
    if rows:
        lines.append("static const RandoEnemyDropLookupEntry kRandoEnemyDropLookup[] = {")
        for r in rows:
            item = r["vanilla_item"]
            if item not in items:
                raise RuntimeError(f"enemy_drop_lookup: unknown item {item!r}")
            if r["drop_kind"] == "small_key":
                drop_kind = "kRandoEnemyDropKind_SmallKey"
            elif r["drop_kind"] == "big_key":
                drop_kind = "kRandoEnemyDropKind_BigKey"
            else:
                raise RuntimeError(f"enemy_drop_lookup: unsupported drop kind {r['drop_kind']!r}")
            lines.append(f"  {{ 0x{int(r['room']):04x}, {int(r['source_slot'])}, "
                         f"{drop_kind}, {int(r['loc_id'])}, ITEM_{item} }},")
        lines.append("};")
    else:
        lines.append("// EMPTY: enemy_drops.gen.yaml absent. Active enemy-drop-check generation fails closed.")
        lines.append("static const RandoEnemyDropLookupEntry kRandoEnemyDropLookup[1] = { { 0, 0, 0, 0, 0 } };")
    lines += [
        "",
        f"#define kRandoEnemyDropLookup_COUNT {len(rows)}",
        f"#define kRandoEnemyDropHceBigKeyLocId 0x{hce_big_key_loc:04x}",
        "",
        "#endif  // ZELDA3_RANDO_ENEMY_DROP_LOOKUP_H_",
    ]
    atomic_write_text(path, "\n".join(lines) + "\n")
    return len(rows)


def emit_enemy_check_lookup(rows, path: Path) -> int:
    """Emit src/rando/enemy_check_lookup.h for ordinary enemy checks."""
    dungeon_rows = sorted(
        [r for r in rows if r.get("domain") == "dungeon"],
        key=lambda r: (r["room"], r["source_slot"]))
    overworld_rows = sorted(
        [r for r in rows if r.get("domain") == "overworld"],
        key=lambda r: (r["area"], r["stage"], r["source_slot"]))
    boss_rows = sorted(
        [r for r in rows if r.get("domain") == "boss"],
        key=lambda r: (r["game_dungeon"], r["room"], r["boss_kind"], r["loc_id"]))
    scripted_rows = sorted(
        [r for r in rows if r.get("domain") == "scripted_spawn"],
        key=lambda r: (
            r["room"], r["parent_source_slot"], r["overlord_type"],
            r["child_index"], r["child_type"]))
    seen = set()
    for r in dungeon_rows:
        key = (int(r["room"]), int(r["source_slot"]))
        if key in seen:
            raise RuntimeError(f"enemy_check_lookup: duplicate row room=0x{key[0]:03x} slot={key[1]}")
        seen.add(key)
    seen.clear()
    for r in overworld_rows:
        key = (int(r["area"]), int(r["stage"]), int(r["source_slot"]))
        if key in seen:
            raise RuntimeError(
                "enemy_check_lookup: duplicate overworld row "
                f"area=0x{key[0]:02x} stage={key[1]} slot={key[2]}")
        seen.add(key)
    seen.clear()
    for r in boss_rows:
        key = (int(r["game_dungeon"]), int(r["room"]), r["boss_kind"])
        if key in seen:
            raise RuntimeError(
                "enemy_check_lookup: duplicate boss row "
                f"game_dungeon={key[0]} room=0x{key[1]:03x} kind={key[2]!r}")
        seen.add(key)
    seen.clear()
    for r in scripted_rows:
        key = (
            int(r["room"]), int(r["parent_source_slot"]), int(r["overlord_type"]),
            int(r["child_index"]), int(r["child_type"]))
        if key in seen:
            raise RuntimeError(
                "enemy_check_lookup: duplicate scripted row "
                f"room=0x{key[0]:03x} parent={key[1]} type=0x{key[2]:02x} child={key[3]}")
        seen.add(key)
    digest = _enemy_check_registry_digest(
        dungeon_rows, overworld_rows, boss_rows, scripted_rows)
    lines = [
        HEADER_BANNER, "",
        "// enemy_check_lookup.h — runtime lookup tables for ordinary Enemy checks.",
        "// Dungeon rows are sorted (dungeon_room_index, source_slot).",
        "// Overworld rows are sorted (overworld_area_index, active_stage, source_slot).",
        "// Boss rows are sorted (game_dungeon, room, boss_kind).",
        "// Scripted rows are sorted (room, parent source-list slot, overlord type, child index).",
        "// Generated from local",
        "// assets/rando/enemy_checks.gen.yaml via assets/scripts/gen_enemy_check_tables.py.",
        "",
        "#ifndef ZELDA3_RANDO_ENEMY_CHECK_LOOKUP_H_",
        "#define ZELDA3_RANDO_ENEMY_CHECK_LOOKUP_H_",
        "",
        "#include \"../types.h\"",
        "",
        "typedef struct RandoEnemyCheckLookupEntry {",
        "  uint16 room;        // 0..319 dungeon room index",
        "  uint8  source_slot; // runtime sprite slot of the source enemy",
        "  uint16 loc_id;      // LOC_* value",
        "  uint8  all_tier_only; // active only when effective enemy_drop_checks=all",
        "} RandoEnemyCheckLookupEntry;",
        "",
        "typedef struct RandoOverworldEnemyCheckLookupEntry {",
        "  uint8  area;        // overworld_area_index",
        "  uint8  stage;       // active kOverworldSpriteOffs stage",
        "  uint8  source_slot; // authored source list slot",
        "  uint16 block;       // sprite_N_word proximity block",
        "  uint16 loc_id;      // LOC_* value",
        "} RandoOverworldEnemyCheckLookupEntry;",
        "",
        "typedef struct RandoBossEnemyCheckLookupEntry {",
        "  uint8  game_dungeon; // cur_palace_index_x2 >> 1 destination dungeon",
        "  uint16 room;         // dungeon_room_index; used for GT miniboss events",
        "  uint8  kind;         // 1=dungeon boss, 2=GT miniboss",
        "  uint16 loc_id;       // LOC_* value",
        "} RandoBossEnemyCheckLookupEntry;",
        "",
        "typedef struct RandoScriptedEnemyCheckLookupEntry {",
        "  uint16 room;               // 0..319 dungeon room index",
        "  uint8  parent_source_slot; // authored room-list index of overlord marker",
        "  uint8  parent_type;        // parent overlord type",
        "  uint8  child_index;        // finite child ordinal from that parent",
        "  uint8  child_type;         // spawned sprite type",
        "  uint16 loc_id;             // LOC_* value",
        "} RandoScriptedEnemyCheckLookupEntry;",
        "",
    ]
    if dungeon_rows:
        lines.append("static const RandoEnemyCheckLookupEntry kRandoEnemyCheckLookup[] = {")
        for r in dungeon_rows:
            all_tier_only = 1 if r.get("all_tier_only") else 0
            lines.append(
                f"  {{ 0x{int(r['room']):04x}, {int(r['source_slot'])}, "
                f"{int(r['loc_id'])}, {all_tier_only} }},")
        lines.append("};")
    else:
        lines.append("// EMPTY: enemy_checks.gen.yaml absent. Active dungeon enemy-check generation fails closed.")
        lines.append("static const RandoEnemyCheckLookupEntry kRandoEnemyCheckLookup[1] = { { 0, 0, 0, 0 } };")
    lines.append("")
    if overworld_rows:
        lines.append("static const RandoOverworldEnemyCheckLookupEntry kRandoOverworldEnemyCheckLookup[] = {")
        for r in overworld_rows:
            lines.append(
                f"  {{ 0x{int(r['area']):02x}, {int(r['stage'])}, "
                f"{int(r['source_slot'])}, 0x{int(r['block']):04x}, {int(r['loc_id'])} }},")
        lines.append("};")
    else:
        lines.append("// EMPTY: no all-tier overworld enemy-check rows.")
        lines.append("static const RandoOverworldEnemyCheckLookupEntry kRandoOverworldEnemyCheckLookup[1] = { { 0, 0, 0, 0, 0 } };")
    lines.append("")
    if boss_rows:
        lines.append("static const RandoBossEnemyCheckLookupEntry kRandoBossEnemyCheckLookup[] = {")
        for r in boss_rows:
            kind = 2 if r.get("boss_kind") == "gt_miniboss" else 1
            lines.append(
                f"  {{ {int(r['game_dungeon'])}, 0x{int(r['room']):04x}, "
                f"{kind}, {int(r['loc_id'])} }},")
        lines.append("};")
    else:
        lines.append("// EMPTY: no all-tier boss/miniboss enemy-check rows.")
        lines.append("static const RandoBossEnemyCheckLookupEntry kRandoBossEnemyCheckLookup[1] = { { 0, 0, 0, 0 } };")
    lines.append("")
    if scripted_rows:
        lines.append("static const RandoScriptedEnemyCheckLookupEntry kRandoScriptedEnemyCheckLookup[] = {")
        for r in scripted_rows:
            lines.append(
                f"  {{ 0x{int(r['room']):04x}, {int(r['parent_source_slot'])}, "
                f"0x{int(r['overlord_type']):02x}, {int(r['child_index'])}, "
                f"0x{int(r['child_type']):02x}, {int(r['loc_id'])} }},")
        lines.append("};")
    else:
        lines.append("// EMPTY: no all-tier scripted-spawn enemy-check rows.")
        lines.append("static const RandoScriptedEnemyCheckLookupEntry kRandoScriptedEnemyCheckLookup[1] = { { 0, 0, 0, 0, 0, 0 } };")
    total_rows = (len(dungeon_rows) + len(overworld_rows) + len(boss_rows)
                  + len(scripted_rows))
    lines += [
        "",
        f"#define kRandoEnemyCheckLookup_COUNT {len(dungeon_rows)}",
        f"#define kRandoOverworldEnemyCheckLookup_COUNT {len(overworld_rows)}",
        f"#define kRandoBossEnemyCheckLookup_COUNT {len(boss_rows)}",
        f"#define kRandoScriptedEnemyCheckLookup_COUNT {len(scripted_rows)}",
        "",
        "// Registry identity for the sidecar/snapshot activation guard (all four",
        "// domains folded — see _enemy_check_registry_digest).",
        f"#define kRandoEnemyCheckRegistryCount {total_rows}",
        f"#define kRandoEnemyCheckRegistryDigest 0x{digest:08x}u",
        "",
        "#endif  // ZELDA3_RANDO_ENEMY_CHECK_LOOKUP_H_",
    ]
    atomic_write_text(path, "\n".join(lines) + "\n")
    return len(rows)


def load_pot_nonpot_drop_counts(path: Path) -> list[tuple[str, int]]:
    """Load the generated non-pot small-key free-grant rows.

    Missing path is the public/assetless build: no pot registry, no pot shuffle,
    and an empty table is correct. If a local key-depth artifact exists but lacks
    the generated section, fail closed rather than silently omitting the
    DUNGEON-key free-grants.
    """
    if not path.exists():
        return []
    doc = load_yaml(path)
    rows = doc.get("nonpot_drops")
    if rows is None:
        raise RuntimeError(
            f"{path} is missing nonpot_drops. Regenerate it with "
            f"assets/scripts/gen_pot_key_depth.py so pot_nonpot_drop_counts.h "
            f"can be derived from the same key-depth artifact."
        )
    out = []
    seen = set()
    for raw in rows or []:
        item = raw["item"]
        count = int(raw["count"])
        if count <= 0:
            raise RuntimeError(f"{path}: nonpot_drops entry {item!r} has non-positive count {count}")
        if item in seen:
            raise RuntimeError(f"{path}: duplicate nonpot_drops item {item!r}")
        seen.add(item)
        out.append((item, count))
    return out


def emit_pot_nonpot_drop_counts(rows, items: dict[str, ItemDef], path: Path) -> int:
    """Emit src/rando/pot_nonpot_drop_counts.h.

    This keeps the C placer's free-grant table derived from the same local
    key-depth artifact that generates POT_KEYS_DUNGEON/POT_KEYS_WILD gates.
    """
    rows = sorted(rows, key=lambda r: items[r[0]].id if r[0] in items else 0xFFFF)
    lines = [
        HEADER_BANNER, "",
        "// pot_nonpot_drop_counts.h — non-pot small-key drops that remain free",
        "// under DUNGEON keys + active pot shuffle. Generated from",
        "// assets/rando/pot_key_depth.gen.yaml via gen_pot_key_depth.py.",
        "",
        "#ifndef ZELDA3_RANDO_POT_NONPOT_DROP_COUNTS_H_",
        "#define ZELDA3_RANDO_POT_NONPOT_DROP_COUNTS_H_",
        "",
        "#include \"../types.h\"",
        "#include \"item_ids.h\"",
        "",
        "typedef struct RandoPotNonpotDropCount {",
        "  uint16 item_id;",
        "  uint8 count;",
        "} RandoPotNonpotDropCount;",
        "",
    ]
    if rows:
        lines.append("static const RandoPotNonpotDropCount kPotNonpotDropCounts[] = {")
        for item, count in rows:
            if item not in items:
                raise RuntimeError(f"pot_nonpot_drop_counts: unknown item {item!r}")
            if count > 0xFF:
                raise RuntimeError(f"pot_nonpot_drop_counts: count for {item!r} exceeds uint8")
            lines.append(f"  {{ ITEM_{item}, {count} }},")
        lines.append("};")
    else:
        lines.append("// EMPTY: pot_key_depth.gen.yaml absent or no non-pot drops need free-granting.")
        lines.append("static const RandoPotNonpotDropCount kPotNonpotDropCounts[1] = { { 0, 0 } };")
    lines += [
        "",
        f"#define kPotNonpotDropCounts_COUNT {len(rows)}",
        "",
        "#endif  // ZELDA3_RANDO_POT_NONPOT_DROP_COUNTS_H_",
    ]
    atomic_write_text(path, "\n".join(lines) + "\n")
    return len(rows)


def emit_chest_lookup(locations: dict[str, LocationDef], path: Path) -> int:
    """Emit src/rando/chest_lookup.h — (dungeon_room, chest_ordinal) -> LOC_*.

    Per tasks.md S6.3 / audit.md S0.3.5+S0.3.7. Cross-references the embedded
    vanilla ROM chest table (assets/chest_data.py) with the canonical
    chest-name catalog at assets/rando/location_registry.yaml to produce a
    flat C array indexed by (room_index, chest_ordinal) for the runtime's
    Rando_ChestDispatch hook at src/player.c:3847.

    The runtime walks kDungeonRoomChests via the same N-th-room-match
    iteration as the in-game OpenChestForItem at src/dungeon.c:5765, so the
    `chest_ordinal` here matches `tile - 0x58` at the dispatch site.

    Returns the number of rows emitted (for diagnostic logging).
    """
    # Validate against the catalog (not the table), so name/type/coverage drift
    # is caught in CI, where the table artifact is absent.
    DEFERRED_CHEST_NAMES = {"Chest Game"}  # §6.8 minigame path
    catalog = chest_data.CHEST_NAME_BY_ROM_ADDR
    catalog_names = {name for (name, _ctype) in catalog.values()}

    # Forward: every catalog chest name resolves to a Chest/BigChest registry entry.
    unknown = []
    for addr, (name, ctype) in catalog.items():
        loc = locations.get(name)
        if loc is None:
            unknown.append((name, addr))
            continue
        if loc.type not in ("Chest", "BigChest"):
            raise RuntimeError(
                "chest_lookup: '%s' (loc %d) has registry type %r != Chest/BigChest"
                % (name, loc.id, loc.type)
            )
    if unknown:
        msg = ", ".join("%s @ 0x%04X" % (n, a) for n, a in unknown[:5])
        raise RuntimeError(
            "chest_lookup: %d chest name(s) absent from location_registry.yaml: %s%s"
            % (len(unknown), msg, " ..." if len(unknown) > 5 else "")
        )

    # Reverse: every registry Chest/BigChest must be in the catalog (minus the
    # deferred Chest Game), else its chest would silently fall through to vanilla.
    missing_in_catalog = []
    for loc in locations.values():
        if loc.type not in ("Chest", "BigChest"):
            continue
        if loc.name in DEFERRED_CHEST_NAMES:
            continue
        if loc.name not in catalog_names:
            missing_in_catalog.append((loc.name, loc.id))
    if missing_in_catalog:
        msg = ", ".join("%s (loc %d)" % (n, i) for n, i in missing_in_catalog[:5])
        raise RuntimeError(
            "chest_lookup: %d registry chest(s) absent from CHEST_NAME_BY_ROM_ADDR: %s%s"
            " — add them with PHP source provenance."
            % (len(missing_in_catalog), msg,
               " ..." if len(missing_in_catalog) > 5 else "")
        )

    # Rows need the generated chest table; absent in CI -> empty lookup (the
    # runtime falls through to vanilla for every chest).
    rows = chest_data.get_chest_lookup_rows()
    if not rows:
        # chest_table.gen.bin is absent. In a ROM-less / CI build this is
        # EXPECTED (the runtime falls through to vanilla for every chest). But in
        # a PLAYABLE build — one where zelda3_assets.dat has been extracted — an
        # empty table SILENTLY makes every chest grant its vanilla item:
        # placement/spoiler stay correct but no chest resolves to its placed item
        # → an unbeatable seed when progression sits in a chest. The two
        # artifacts come from the SAME extraction, so the split state (assets
        # present, chest bin missing — e.g. a stale incremental build) is always
        # a bug. Fail closed in that context instead of fail-open.
        import sys as _sys
        # REPO (module-global, derived from __file__) is cwd-INDEPENDENT, so the
        # guard works regardless of where the codegen is invoked from — a
        # cwd-relative probe would fail OPEN from another cwd.
        if (REPO / "zelda3_assets.dat").exists():
            _sys.stderr.write(
                "ERROR: chest_table.gen.bin is missing but zelda3_assets.dat is "
                "present — a playable build would grant EVERY chest its vanilla "
                "item (unbeatable when progression is in a chest). Re-run asset "
                "extraction (python assets/restool.py --extract-from-rom) to "
                "regenerate chest_table.gen.bin, then rebuild.\n")
            _sys.exit(2)  # hard build failure (caller reads the return as a count)
        _write_empty_chest_lookup(path)
        print(
            "  NOTE: chest_lookup.h emitted EMPTY (chest_table.gen.bin absent; "
            "expected in a ROM-less / CI build). Run asset extraction for a "
            "playable build."
        )
        return 0

    # Sort by (room, ordinal) so the C-side binary search has a sorted key.
    rows.sort(key=lambda r: (r[0], r[1]))

    lines = [
        HEADER_BANNER,
        "",
        "// chest_lookup.h — (dungeon_room_index, chest_ordinal) -> LOC_* table.",
        "//",
        "// Hooked from src/player.c:3847 Link_PerformOpenChest via",
        "// Rando_ChestDispatch in src/rando/rando.c. `chest_ordinal` is the",
        "// 0-based in-room chest index `tile - 0x58` matching src/dungeon.c:5765",
        "// OpenChestForItem's iteration (N-th matching room in",
        "// kDungeonRoomChests).",
        "//",
        "// Source: in-room ordinals from the vanilla chest table (168 x 3 bytes),",
        "// read at codegen time from the generated chest_table.gen.bin; chest",
        "// names from ALTTPR PHP app/Region/Standard/**.php, cross-checked against",
        "// location_registry.yaml.",
        "",
        "#ifndef ZELDA3_RANDO_CHEST_LOOKUP_H_",
        "#define ZELDA3_RANDO_CHEST_LOOKUP_H_",
        "",
        "#include \"../types.h\"",
        "#include \"location_ids.h\"",
        "",
        "typedef struct RandoChestLookupEntry {",
        "  uint16 room;     // 0..319 dungeon room index",
        "  uint8  ordinal;  // 0..5; in-room chest index = (tile - 0x58)",
        "  uint16 loc_id;   // LOC_*",
        "} RandoChestLookupEntry;",
        "",
        "// %d entries, sorted by (room, ordinal). 164 of 165 ALTTPR chest" % len(rows),
        "// locations (Chest Game @ ROM 0xEDA8 is the minigame path; dispatch is",
        "// tasks.md S6.8). 4 ROM chest-table entries have no ALTTPR location",
        "// (one entry each in rooms 91, 126, 179, 301 - small-key/rupee chests",
        "// that ALTTPR does not expose as shuffleable; rooms 126 and 179 each",
        "// have ONE mapped chest at ord 0 plus the unmapped ord-1). The unmapped",
        "// entries fall through to vanilla at runtime via chest_lookup()'s",
        "// 0xFFFF return.",
        "static const RandoChestLookupEntry kRandoChestLookup[] = {",
    ]
    for room, ord_, name, ctype, item, big, addr in rows:
        loc = locations[name]
        token = sanitize_for_define(name)
        big_mark = "BigChest" if big else "Chest"
        lines.append(
            "  { %3d, %d, LOC_%s },  // ROM 0x%04X item=0x%02X %s '%s'"
            % (room, ord_, token, addr, item, big_mark, name)
        )
    lines.append("};")
    lines.append("")
    lines.append("#define kRandoChestLookup_COUNT %d" % len(rows))
    lines.append("")
    lines.append("#endif  // ZELDA3_RANDO_CHEST_LOOKUP_H_")
    atomic_write_text(path, "\n".join(lines) + "\n")
    return len(rows)


def emit_icon_atlas(icon_atlas_path: Path, out_header: Path) -> int:
    """Emit src/rando/icon_atlas.h — kHashIconAtlas[N] tile-index table.

    Per tasks.md S9.4b: the 5-icon visual hash widget computes
    `index_i = SHA-256(share_string_binary)[i] mod N` for i in 0..4, then
    emits 5 OAM tiles using kHashIconAtlas[index_i] as the tile index.

    The atlas YAML defines a curated icon list (whatever pool the YAML
    pins; not constrained to 32). Bumping the atlas changes the icon
    output for every existing share string and therefore advances
    kGeneratorVersion (per randomizer-ui spec).

    Returns the entry count (for diagnostic logging).
    """
    if not icon_atlas_path.exists():
        raise RuntimeError(
            "emit_icon_atlas: missing source YAML at %s" % icon_atlas_path
        )
    with open(icon_atlas_path, "r", encoding="utf-8") as f:
        atlas_doc = yaml.safe_load(f)
    icons = atlas_doc.get("icons", [])
    if not icons:
        raise RuntimeError(
            "emit_icon_atlas: %s has no 'icons:' entries — needs at least 1"
            % icon_atlas_path
        )
    entries = []
    for i, entry in enumerate(icons):
        if not isinstance(entry, dict):
            raise RuntimeError(
                "emit_icon_atlas: entry %d in %s is not a mapping" % (i, icon_atlas_path)
            )
        if "tile" not in entry:
            raise RuntimeError(
                "emit_icon_atlas: entry %d (%r) missing required 'tile' key"
                % (i, entry.get("name", "(unnamed)"))
            )
        tile = entry["tile"]
        if not isinstance(tile, int) or tile < 0 or tile > 0xff:
            raise RuntimeError(
                "emit_icon_atlas: entry %d (%r) tile %r out of 0..0xff range"
                % (i, entry.get("name", "(unnamed)"), tile)
            )
        name = entry.get("name", "icon_%d" % i)
        entries.append((name, tile))

    lines = [
        HEADER_BANNER,
        "",
        "// icon_atlas.h - 5-icon visual hash widget tile table.",
        "//",
        "// Per tasks.md S9.4b / randomizer-ui spec: the widget computes",
        "//   index_i = SHA-256(share_string_binary)[i] mod kHashIconAtlasSize",
        "// for i in 0..4 and emits 5 OAM tiles via kHashIconAtlas[index_i].",
        "//",
        "// CRITICAL: the hash input is share_string_binary, NOT settings_hash.",
        "// Deriving from settings_hash gives every seed with the same settings",
        "// the same icons (architectural error caught in spec round 5).",
        "//",
        "// Source: assets/rando/icon_atlas.yaml. Atlas size and entry order",
        "// is the registry pin per 'randomizer-ui / Atlas size is registry-",
        "// pinned'. Bumping the atlas advances kGeneratorVersion.",
        "",
        "#ifndef ZELDA3_RANDO_ICON_ATLAS_H_",
        "#define ZELDA3_RANDO_ICON_ATLAS_H_",
        "",
        "#include \"../types.h\"",
        "",
        "// %d entries; index_i is masked by kHashIconAtlasSize at runtime." % len(entries),
        "static const uint8 kHashIconAtlas[] = {",
    ]
    for i, (name, tile) in enumerate(entries):
        lines.append("  0x%02x,  // [%d] %s" % (tile, i, name))
    lines.append("};")
    lines.append("")
    lines.append("#define kHashIconAtlasSize %d" % len(entries))
    lines.append("")
    lines.append("#endif  // ZELDA3_RANDO_ICON_ATLAS_H_")
    atomic_write_text(out_header, "\n".join(lines) + "\n")
    return len(entries)


def emit_direct_grant_icons(
    icons_yaml_path: Path,
    items: dict[str, ItemDef],
    out_header: Path,
) -> int:
    """Emit src/rando/direct_grant_icons.h — kDirectGrantIcons[ITEM__COUNT] table.

    Phase B Slice 9 — add-rando-confirmation-icons. The §7.6 direct-grant
    helper looks up the granted item id in this table. gfx=0 entries mean
    "audio-only" and the helper falls back to sound + HUD only (Phase A
    behavior). gfx!=0 entries spawn an icon-receipt ancilla that DMAs the
    item's animated-sprite bundle (exactly like the vanilla receive-item
    pickup) and draws OAM chars 0x24/0x34.

    Each entry pins three values, all derived from the vanilla receive-item
    tables in src/misc.c / src/sprite_main.c, indexed by the LttP receive code
    whose kMemoryLocationToGiveItemTo cell matches the direct-grant's
    destination RAM cell:

      gfx       - DecodeAnimatedSpriteTile_variable() index = kReceiveItemGfx
                  [lttp]. gfx == 0 is the AUDIO-ONLY sentinel (no icon).
      big       - the OAM size byte = kReceiveItem_Tab1[lttp] (0 -> 8x16
                  two-tile, 2 -> single tile).
      oam_flags - the OAM palette/priority byte = (kWishPond2_OamFlags[lttp]
                  * 2) | 0x30, matching Ancilla_ReceiveItem_Draw.

    The table is dense (sized to ITEM__COUNT) and indexed by item id so
    callers can do `kDirectGrantIcons[item_id]` without a search.

    Returns the count of items WITH a non-zero gfx (for diagnostic logging).
    """
    entries: dict[int, tuple[int, int, int, str]] = {}  # id -> (gfx, big, oam, name)
    if icons_yaml_path.exists():
        with open(icons_yaml_path, "r", encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        raw = doc.get("icons", {}) or {}
        if not isinstance(raw, dict):
            raise RuntimeError(
                "emit_direct_grant_icons: %s 'icons:' must be a mapping"
                % icons_yaml_path
            )
        for name, entry in raw.items():
            if name not in items:
                raise RuntimeError(
                    "emit_direct_grant_icons: %s references unknown item %r"
                    % (icons_yaml_path, name)
                )
            if not isinstance(entry, dict):
                raise RuntimeError(
                    "emit_direct_grant_icons: entry %r is not a mapping" % name
                )
            gfx = entry.get("gfx", 0)
            big = entry.get("big", 0)
            oam_flags = entry.get("oam_flags", 0)
            for fld, v in (("gfx", gfx), ("big", big), ("oam_flags", oam_flags)):
                if not isinstance(v, int) or v < 0 or v > 0xff:
                    raise RuntimeError(
                        "emit_direct_grant_icons: entry %r %s %r out of 0..0xff"
                        % (name, fld, v)
                    )
            entries[items[name].id] = (gfx, big, oam_flags, name)

    item_count = (max(it.id for it in items.values()) + 1) if items else 0

    lines = [
        HEADER_BANNER,
        "",
        "// direct_grant_icons.h - per-item icon table for the §7.6 direct-grant",
        "// confirmation ancilla (Phase B Slice 9 — add-rando-confirmation-icons).",
        "//",
        "// Indexed by item id. gfx=0 entries cause the helper to fall back to",
        "// audio + HUD only (Phase A behavior preserved). gfx!=0 entries spawn",
        "// the kAncillaType_RandoIconReceipt ancilla, which DMAs the item's",
        "// receive-animation sprite bundle and draws it above Link.",
        "//",
        "// Source: assets/rando/direct_grant_icons.yaml. gfx/big/oam_flags are",
        "// derived from the vanilla receive-item tables (kReceiveItemGfx,",
        "// kReceiveItem_Tab1, kWishPond2_OamFlags) indexed by the item's LttP",
        "// receive code, so each icon matches the vanilla pickup animation.",
        "",
        "#ifndef ZELDA3_RANDO_DIRECT_GRANT_ICONS_H_",
        "#define ZELDA3_RANDO_DIRECT_GRANT_ICONS_H_",
        "",
        "#include \"../types.h\"",
        "",
        "typedef struct DirectGrantIconEntry {",
        "  uint8 gfx;        // DecodeAnimatedSpriteTile_variable index; 0 = audio-only.",
        "  uint8 big;        // OAM size byte (0 = 8x16 two-tile, 2 = single tile).",
        "  uint8 oam_flags;  // OAM palette/priority byte.",
        "} DirectGrantIconEntry;",
        "",
        "// Indexed by item id. ITEM__COUNT-sized so direct subscripting is safe.",
        "static const DirectGrantIconEntry kDirectGrantIcons[%d] = {" % item_count,
    ]
    for it in sorted(items.values(), key=lambda i: i.id):
        if it.id in entries:
            gfx, big, oam_flags, name = entries[it.id]
            lines.append(
                "  [%d] = { 0x%02x, 0x%02x, 0x%02x },  // %s"
                % (it.id, gfx, big, oam_flags, name)
            )
    lines.append("};")
    lines.append("")
    lines.append("#endif  // ZELDA3_RANDO_DIRECT_GRANT_ICONS_H_")

    atomic_write_text(out_header, "\n".join(lines) + "\n")
    return sum(1 for (gfx, _b, _o, _name) in entries.values() if gfx != 0)


def emit_logic_data(
    locations: dict[str, LocationDef],
    regions: dict[str, RegionDef],
    edges: list[EdgeDef],
    location_predicates: dict[str, dict[str, bytes]],
    edge_predicates: list[bytes],
    path: Path,
    items: dict[str, ItemDef] | None = None,
    logic_loc_preds: dict[str, LocationDef] | None = None,
    compiled_overrides: dict[int, list[tuple[str, dict[str, bytes]]]] | None = None,
    compiled_world_state_edges: dict[int, list[tuple[EdgeDef, bytes]]] | None = None,
    trick_status_rows: list[dict] | None = None,
    glitch_status_rows: list[dict] | None = None,
    boss_kill_predicates: list[bytes] | None = None,
    dungeon_vanilla_boss: list[int] | None = None,
    cave_source_predicates: list[bytes] | None = None,
    door_vm_preds: list[bytes] | None = None,
    door_portal_rows: list[tuple] | None = None,
    door_pot_rows: list[dict] | None = None,
    door_pot_bridge_digest: int = 0,
    door_enemy_drop_rows: list[dict] | None = None,
    door_enemy_drop_bridge_digest: int = 0,
    door_enemy_check_rows: list[dict] | None = None,
    door_enemy_check_bridge_digest: int = 0,
    ow_meta: dict | None = None,
    ow_non_warp_edges: int = 0,
    ow_regions_first: int = 0,
    souls_baked: bool = False,
    soul_pin_rooms: list[int] | None = None,
):
    out = [HEADER_BANNER, "", "#include \"../types.h\"", "#include \"rando_logic.h\"", "#include \"location_ids.h\"", "#include \"item_ids.h\"", ""]
    emit_item_grant_metadata(items or {}, out)
    region_ids = _region_ids_for_codegen(regions) if regions else []
    region_index = {rid: i for i, rid in enumerate(region_ids)}
    # Predicate stream — concatenated; LocationDef references offset+length.
    stream = bytearray()
    location_offsets = {}
    for loc in sorted(locations.values(), key=lambda l: l.id):
        preds = location_predicates.get(loc.name, {})
        cr = preds.get("can_reach", b"\x0c\x00")  # default TRUE = AND with 0 children = 0x0c 0x00
        cp = preds.get("can_place", b"\x0c\x00")
        aa = preds.get("always_allow", b"\x0d\x00")  # default FALSE = OR with 0 children = 0x0d 0x00
        location_offsets[loc.id] = {
            "can_reach": (len(stream), len(cr)),
        }
        stream += cr
        location_offsets[loc.id]["can_place"] = (len(stream), len(cp))
        stream += cp
        location_offsets[loc.id]["always_allow"] = (len(stream), len(aa))
        stream += aa
    edge_offsets = []
    for ep in edge_predicates:
        edge_offsets.append((len(stream), len(ep)))
        stream += ep

    # Phase B Slice 2 — per-world-state location predicate overrides.
    # For each (world_state_id, loc_name, region_str_or_None, encoded) tuple
    # in compiled_overrides, append the three encoded predicates to the
    # shared `stream` and record their offsets. Capture a per-override
    # region_id when the world-state YAML moves the location (e.g.,
    # Ether Tablet East↔West in Inverted).
    # world_state_id → list[ (loc_id, region_override_id, cr_off, cp_off, aa_off) ]
    override_offsets: dict[int, list[tuple[int, int, tuple[int, int], tuple[int, int], tuple[int, int]]]] = {}
    rid_for_overrides = region_index
    # Map location_id → base region index (from logic_loc_region resolution
    # used in the base predicate emission above). Used to
    # detect when an override's region matches the base — in that case we
    # emit the 0xFFFF sentinel so the runtime takes the "no region change"
    # fast path. Avoids the 100%-overridden anti-pattern where the
    # sentinel path is dead code.
    base_region_idx_by_loc_id: dict[int, int] = {}
    for _loc in sorted(locations.values(), key=lambda l: l.id):
        _off = location_offsets.get(_loc.id, {})
        # Reconstruct what the base emission picked: same lookup as the
        # main loop in `emit_logic_data` (logic_loc_region → registry
        # default → 0xFFFF).
        _region_name = (logic_loc_preds or {}).get(_loc.name)
        _region_name = _region_name.region if _region_name else None
        if _region_name and _region_name in rid_for_overrides:
            base_region_idx_by_loc_id[_loc.id] = rid_for_overrides[_region_name]
        else:
            base_region_idx_by_loc_id[_loc.id] = 0xFFFF
    if compiled_overrides:
        for ws_id, entries in compiled_overrides.items():
            for loc_name, region_str, encoded in entries:
                if loc_name not in locations:
                    continue  # already warned upstream
                loc_id = locations[loc_name].id
                # Resolve region_override. 0xFFFF means "no region change"
                # (use base region from kRandoLocations). Else the override
                # moves the location to a different region for this
                # world state.
                override_region_id = 0xFFFF
                if region_str is not None and region_str in rid_for_overrides:
                    candidate_idx = rid_for_overrides[region_str]
                    base_idx = base_region_idx_by_loc_id.get(loc_id, 0xFFFF)
                    # only record an override when it differs
                    # from the base region. Matching cases emit 0xFFFF so
                    # the runtime sentinel path is exercised.
                    if candidate_idx != base_idx:
                        override_region_id = candidate_idx
                cr = encoded.get("can_reach", b"\x0c\x00")
                cp = encoded.get("can_place", b"\x0c\x00")
                aa = encoded.get("always_allow", b"\x0d\x00")
                cr_off = (len(stream), len(cr)); stream += cr
                cp_off = (len(stream), len(cp)); stream += cp
                aa_off = (len(stream), len(aa)); stream += aa
                override_offsets.setdefault(ws_id, []).append(
                    (loc_id, override_region_id, cr_off, cp_off, aa_off)
                )

    # Phase B Slice 2 — per-world-state edge predicates (Inverted overworld
    # edges, mirror-back paths). Append to the same predicate stream.
    # world_state_id → list[ (from_region_idx, to_region_idx, one_way, (off, len)) ]
    ws_edge_offsets: dict[int, list[tuple[int, int, int, tuple[int, int]]]] = {}
    if compiled_world_state_edges:
        for ws_id, entries in compiled_world_state_edges.items():
            for edge_def, encoded_pred in entries:
                ep_offset = (len(stream), len(encoded_pred))
                stream += encoded_pred
                from_idx = region_index.get(edge_def.from_, 0xFFFF)
                to_idx = region_index.get(edge_def.to, 0xFFFF)
                ws_edge_offsets.setdefault(ws_id, []).append(
                    (from_idx, to_idx, 1 if edge_def.one_way else 0, ep_offset)
                )

    # Boss-shuffle runtime — per-boss kill predicates (OP_CAN_KILL_BOSS). Append
    # to the same stream; kRandoBossKillPred[] references (offset, length).
    boss_kill_offsets: list[tuple[int, int]] = []
    for enc in (boss_kill_predicates or []):
        boss_kill_offsets.append((len(stream), len(enc)))
        stream += enc

    # Door shuffle — Vm-leaf predicates (door_predicates.gen.json order) +
    # portal seeding gate predicates. Appended to the same stream.
    door_vm_offsets: list[tuple[int, int]] = []
    for enc in (door_vm_preds or []):
        door_vm_offsets.append((len(stream), len(enc)))
        stream += enc
    door_portal_offsets: list[tuple] = []
    for (dgn, is_drop, door_region, fork_region, enc, pname) in (door_portal_rows or []):
        off, length = (len(stream), len(enc)) if enc else (0, 0)
        stream += enc
        door_portal_offsets.append((dgn, is_drop, door_region, fork_region, off, length, pname))
    door_pot_offsets: list[dict] = []
    door_pot_regions: list[int] = []
    for row in (door_pot_rows or []):
        enc = row.get("pred", b"")
        off, length = (len(stream), len(enc)) if enc else (0, 0)
        stream += enc
        out_row = dict(row)
        out_row["region_first"] = len(door_pot_regions)
        out_row["pred_off"] = off
        out_row["pred_len"] = length
        door_pot_regions.extend(row.get("regions", []))
        door_pot_offsets.append(out_row)
    door_enemy_drop_offsets: list[dict] = []
    for row in (door_enemy_drop_rows or []):
        enc = row.get("pred", b"")
        off, length = (len(stream), len(enc)) if enc else (0, 0)
        stream += enc
        out_row = dict(row)
        out_row["pred_off"] = off
        out_row["pred_len"] = length
        door_enemy_drop_offsets.append(out_row)
    door_enemy_check_offsets: list[dict] = []
    door_enemy_check_regions: list[int] = []
    for row in (door_enemy_check_rows or []):
        enc = row.get("pred", b"")
        off, length = (len(stream), len(enc)) if enc else (0, 0)
        stream += enc
        out_row = dict(row)
        out_row["region_first"] = len(door_enemy_check_regions)
        out_row["pred_off"] = off
        out_row["pred_len"] = length
        door_enemy_check_regions.extend(row.get("regions", []))
        door_enemy_check_offsets.append(out_row)

    # Cave entrance-shuffle source gates (entrance_registry.yaml order). These
    # are AND-ed into any cave/dungeon target reached through the source cave
    # slot. len 0 means the source region alone is enough.
    cave_source_offsets: list[tuple[int, int]] = []
    for enc in (cave_source_predicates or []):
        off, length = (len(stream), len(enc)) if enc else (0, 0)
        stream += enc
        cave_source_offsets.append((off, length))

    # Emit the stream as a uint8 array.
    out.append("// Predicate bytecode stream — concatenated per the encoding documented in")
    out.append("// assets/rando_logic_gen.py. Locations and edges reference (offset, length).")
    if not stream:
        stream = bytes([0])  # avoid zero-length array
    out.append(f"const uint8 kRandoPredicateStream[{len(stream)}] = {{")
    rows = []
    for i in range(0, len(stream), 16):
        chunk = stream[i:i+16]
        rows.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    out.extend(rows)
    out.append("};")
    out.append(f"const uint32 kRandoPredicateStreamSize = {len(stream)};")
    out.append("")

    # Boss-shuffle runtime — OP_CAN_KILL_BOSS dispatch tables.
    out.append("// Boss-shuffle runtime — per-boss kill predicate table (OP_CAN_KILL_BOSS).")
    out.append("// Indexed by boss-pool index (kBoss_* in src/rando/shuffle_boss.c).")
    out.append("// Type definition: rando_logic.h::RandoBossKillPred.")
    if boss_kill_offsets:
        out.append(f"const RandoBossKillPred kRandoBossKillPred[{len(boss_kill_offsets)}] = {{")
        for bi, (off, length) in enumerate(boss_kill_offsets):
            out.append(f"  {{ {off}, {length} }},  // boss {bi}")
        out.append("};")
        out.append(f"const uint32 kRandoBossKillPredCount = {len(boss_kill_offsets)};")
    else:
        out.append("const RandoBossKillPred kRandoBossKillPred[1] = { { 0, 0 } };")
        out.append("const uint32 kRandoBossKillPredCount = 0;")
    out.append("")

    # Cave entrance-shuffle source predicates (entrance_registry.yaml order).
    out.append("// Cave entrance-shuffle source gates (entrance_registry.yaml interiors order).")
    if cave_source_offsets:
        out.append(f"const RandoCaveSourcePred kRandoCaveSourcePreds[{len(cave_source_offsets)}] = {{")
        for idx, (off, length) in enumerate(cave_source_offsets):
            out.append(f"  {{ {off}, {length} }},  // cave interior {idx}")
        out.append("};")
        out.append(f"const uint32 kRandoCaveSourcePredsCount = {len(cave_source_offsets)};")
    else:
        out.append("const RandoCaveSourcePred kRandoCaveSourcePreds[1] = { { 0, 0 } };")
        out.append("const uint32 kRandoCaveSourcePredsCount = 0;")
    out.append("")

    # Door shuffle — Vm-pred + portal-gate tables (rando_logic.h types).
    out.append("// Door-shuffle Vm-leaf predicates (door_predicates.gen.json order).")
    if door_vm_offsets:
        out.append(f"const RandoDoorVmPred kDoorVmPreds[{len(door_vm_offsets)}] = {{")
        for off, length in door_vm_offsets:
            out.append(f"  {{ {off}, {length} }},")
        out.append("};")
        out.append(f"const uint32 kDoorVmPredsCount = {len(door_vm_offsets)};")
    else:
        out.append("const RandoDoorVmPred kDoorVmPreds[1] = { { 0, 0 } };")
        out.append("const uint32 kDoorVmPredsCount = 0;")
    out.append("")
    out.append("// Door-shuffle portal seeding gates (door_portals.yaml).")
    if door_portal_offsets:
        out.append(f"const RandoDoorPortalGate kDoorPortalGates[{len(door_portal_offsets)}] = {{")
        for (dgn, is_drop, door_region, fork_region, off, length, pname) in door_portal_offsets:
            out.append(f"  {{ {dgn}, {is_drop}, {door_region}, {fork_region}, {off}, {length} }},  // {pname}")
        out.append("};")
        out.append(f"const uint32 kDoorPortalGatesCount = {len(door_portal_offsets)};")
    else:
        out.append("const RandoDoorPortalGate kDoorPortalGates[1] = { { 0, 0, 0, 0xFFFF, 0, 0 } };")
        out.append("const uint32 kDoorPortalGatesCount = 0;")
    out.append("")
    out.append("// Door x pot-shuffle bridge rows (generated from local pot artifacts).")
    if door_pot_offsets:
        out.append(f"const RandoDoorPotLocation kRandoDoorPotLocations[{len(door_pot_offsets)}] = {{")
        for r in door_pot_offsets:
            out.append(
                "  { %d, %d, %du, %d, 0x%04x, %d, %d, 0x%02x, %d },  // %s"
                % (r["loc_id"], r["region_first"], r["pred_off"], r["pred_len"],
                   r["drop_index"], r["dungeon"], r["min_tier"], r["flags"],
                   len(r.get("regions", [])), r["name"])
            )
        out.append("};")
        out.append(f"const uint16 kRandoDoorPotRegions[{len(door_pot_regions)}] = {{")
        for i in range(0, len(door_pot_regions), 12):
            chunk = door_pot_regions[i:i + 12]
            out.append("  " + ", ".join(str(r) for r in chunk) + ",")
        out.append("};")
        out.append(f"const uint32 kRandoDoorPotLocationsCount = {len(door_pot_offsets)};")
    else:
        out.append("const RandoDoorPotLocation kRandoDoorPotLocations[1] = { { 0, 0, 0, 0, 0xFFFF, 0, 0, 0, 0 } };")
        out.append("const uint16 kRandoDoorPotRegions[1] = { 0xFFFF };")
        out.append("const uint32 kRandoDoorPotLocationsCount = 0;")
    out.append(f"const uint32 kRandoDoorPotBridgeDigest = 0x{door_pot_bridge_digest & 0xFFFFFFFF:08x}u;")
    out.append("")
    out.append("// Door x enemy-drop bridge rows (generated from local enemy-drop artifacts).")
    if door_enemy_drop_offsets:
        out.append(f"const RandoDoorEnemyDropLocation kRandoDoorEnemyDropLocations[{len(door_enemy_drop_offsets)}] = {{")
        for r in door_enemy_drop_offsets:
            out.append(
                "  { %d, 0x%04x, %du, %d, 0x%04x, %d },  // %s"
                % (r["loc_id"], r["region"], r["pred_off"], r["pred_len"],
                   r["drop_index"], r["dungeon"], r["name"])
            )
        out.append("};")
        out.append(f"const uint32 kRandoDoorEnemyDropLocationsCount = {len(door_enemy_drop_offsets)};")
    else:
        out.append("const RandoDoorEnemyDropLocation kRandoDoorEnemyDropLocations[1] = { { 0, 0xFFFF, 0, 0, 0xFFFF, 0 } };")
        out.append("const uint32 kRandoDoorEnemyDropLocationsCount = 0;")
    out.append(f"const uint32 kRandoDoorEnemyDropBridgeDigest = 0x{door_enemy_drop_bridge_digest & 0xFFFFFFFF:08x}u;")
    out.append("")
    out.append("// Door x ordinary enemy-check bridge rows (generated from local enemy-check artifacts).")
    if door_enemy_check_offsets:
        out.append(f"const RandoDoorEnemyCheckLocation kRandoDoorEnemyCheckLocations[{len(door_enemy_check_offsets)}] = {{")
        for r in door_enemy_check_offsets:
            out.append(
                "  { %d, %d, %du, %d, %d, %d, %d },  // %s"
                % (r["loc_id"], r["region_first"], r["pred_off"], r["pred_len"],
                   r["dungeon"], r["min_tier"], len(r.get("regions", [])), r["name"])
            )
        out.append("};")
        out.append(f"const uint16 kRandoDoorEnemyCheckRegions[{len(door_enemy_check_regions)}] = {{")
        for i in range(0, len(door_enemy_check_regions), 12):
            chunk = door_enemy_check_regions[i:i + 12]
            out.append("  " + ", ".join(str(r) for r in chunk) + ",")
        out.append("};")
        out.append(f"const uint32 kRandoDoorEnemyCheckLocationsCount = {len(door_enemy_check_offsets)};")
    else:
        out.append("const RandoDoorEnemyCheckLocation kRandoDoorEnemyCheckLocations[1] = { { 0, 0, 0, 0, 0, 0, 0 } };")
        out.append("const uint16 kRandoDoorEnemyCheckRegions[1] = { 0xFFFF };")
        out.append("const uint32 kRandoDoorEnemyCheckLocationsCount = 0;")
    out.append(f"const uint32 kRandoDoorEnemyCheckBridgeDigest = 0x{door_enemy_check_bridge_digest & 0xFFFFFFFF:08x}u;")
    out.append("")
    out.append("// dungeon-id (HCE=0..GT=12) -> vanilla boss-pool index (0xFF = no boss).")
    out.append("// Mirrors src/rando/shuffle_boss.c kBossVanilla; OP_CAN_KILL_BOSS fallback")
    out.append("// when no per-seed boss assignment is installed.")
    dvb = list(dungeon_vanilla_boss) if dungeon_vanilla_boss else [0xFF] * 13
    out.append(f"const uint8 kRandoDungeonVanillaBoss[{len(dvb)}] = {{")
    out.append("  " + ", ".join(f"0x{b & 0xFF:02x}" for b in dvb) + ",")
    out.append("};")
    out.append("")

    # add-enemy-souls — kill-gated-room soul data (soul_rooms.gen.yaml).
    # kRandoSoulRoomsBaked=0 means this build carries no kill-room soul
    # requirements: BuildItemPool refuses souls_shuffle=all seeds (fail closed,
    # like the pot/enemy registries). Pin rooms are the kill-gated + forced
    # key-drop rooms whose logic terms name VANILLA species — EnemyShuffle_
    # PickDungeon returns the vanilla type there under the enemies tier.
    out.append("// add-enemy-souls — souls_shuffle=all bake flag + enemy-shuffle pin rooms.")
    pins = sorted(set(soul_pin_rooms or []))
    if souls_baked and not pins:
        raise RuntimeError("souls_baked with empty pin rooms — soul_rooms.gen.yaml corrupt")
    out.append(f"const uint8 kRandoSoulRoomsBaked = {1 if souls_baked else 0};")
    if pins:
        out.append(f"const uint16 kRandoSoulPinRooms[{len(pins)}] = {{")
        for i in range(0, len(pins), 12):
            out.append("  " + ", ".join(f"0x{r:03x}" for r in pins[i:i + 12]) + ",")
        out.append("};")
        out.append(f"const uint16 kRandoSoulPinRoomsCount = {len(pins)};")
    else:
        out.append("// EMPTY: soul_rooms.gen.yaml absent. souls_shuffle=all generation fails closed.")
        out.append("const uint16 kRandoSoulPinRooms[1] = { 0xFFFF };")
        out.append("const uint16 kRandoSoulPinRoomsCount = 0;")
    out.append("")

    # LocationDef table — typedef lives in rando_logic.h; emit only the array.
    out.append("// Location table — one row per location_registry.yaml entry.")
    out.append("// Type definition: rando_logic.h::RandoLocationDef.")
    out.append(f"const RandoLocationDef kRandoLocations[{len(locations) or 1}] = {{")
    if not locations:
        out.append("  {0, 0, 0xFFFF, 0, 0, 0, 0, 0, 0, 0, 0, 0xff},  // placeholder for zero-length array compatibility")
    else:
        # logic.yaml's location entries carry an optional `region:` field.
        # The location_registry.yaml's `region:` field is descriptive only
        # (free-form string, not necessarily a logic.yaml region id) — we
        # prefer logic.yaml when present.
        logic_loc_region = {name: lp.region for name, lp in (logic_loc_preds or {}).items()}
        for loc in sorted(locations.values(), key=lambda l: l.id):
            offsets = location_offsets[loc.id]
            cr_off, cr_len = offsets["can_reach"]
            cp_off, cp_len = offsets["can_place"]
            aa_off, aa_len = offsets["always_allow"]
            type_id = _location_type_id(loc.type)
            ws_mask = _world_state_mask(loc.world_state_filter)
            # Resolve vanilla_item_id from the items registry.
            vanilla_id = 0
            if items and loc.vanilla_item and loc.vanilla_item in items:
                vanilla_id = items[loc.vanilla_item].id
            elif loc.vanilla_item:
                # Reference to an unknown item — emit 0 but flag in the comment.
                pass
            # Resolve region_id. The logic.yaml may have declared a region for
            # this location; resolve to numeric index. 0xFFFF = not declared.
            # The well-formedness pass in main() warns about non-allowlisted
            # types that hit the 0xFFFF branch — that's the silent-bypass
            # guard preventing the King Zora region-binding regression.
            region_name = logic_loc_region.get(loc.name)
            if region_name and region_name in region_index:
                region_id = region_index[region_name]
            else:
                region_id = 0xFFFF
            out.append(f"  {{ {loc.id}, {vanilla_id}, 0x{region_id:04x}, 0, {cr_off}u, {cr_len}, {cp_off}u, {cp_len}, {aa_off}u, {aa_len}, {type_id}, 0x{ws_mask:02x} }},  // {loc.name} (vanilla: {loc.vanilla_item}, region: {region_name or '-'})")
    out.append("};")
    out.append(f"const uint32 kRandoLocationsCount = {len(locations)};")
    # add-rando-grass-rock-shuffle — reachability fast-path boundary. Terrain
    # locations (LOCTYPE_Grass=20 / Rock=21) are a CONTIGUOUS SUFFIX of the
    # id-sorted array (their id block 3072+ is the registry's highest), so when
    # both terrain axes are off the reachability expansion can stop at this
    # index instead of visiting 3943 inert rows every pass (a door-shuffle
    # corpus seed's generation time otherwise crosses the 120 s budget). Assert
    # the contiguity so a future family inserted past terrain can't silently
    # break the fast-path (it would need its own boundary or a re-sort).
    _sorted = sorted(locations.values(), key=lambda l: l.id)
    _terr = {"Grass", "Rock"}
    _first_terr = next((i for i, l in enumerate(_sorted) if l.type in _terr), len(_sorted))
    for _i in range(_first_terr, len(_sorted)):
        if _sorted[_i].type not in _terr:
            sys.exit("rando_logic_gen: terrain locations are not a contiguous "
                     "suffix of the id-sorted registry — the reachability "
                     "fast-path boundary (kRandoNonTerrainLocationsCount) would "
                     f"be wrong (row {_i} id={_sorted[_i].id} type={_sorted[_i].type})")
    out.append(f"const uint32 kRandoNonTerrainLocationsCount = {_first_terr};")
    out.append("")
    # RegionDef table — typedef lives in rando_logic.h.
    out.append("// Region table — one row per logic.yaml `regions:` entry. Phase A may emit empty.")
    out.append("// Type definition: rando_logic.h::RandoRegionDef.")
    region_list = [regions[rid] for rid in region_ids]
    if region_list:
        out.append(f"const RandoRegionDef kRandoRegions[{len(region_list)}] = {{")
        for r in region_list:
            rid = region_index[r.id]
            pid = region_index[r.parent] if r.parent and r.parent in region_index else 0xFFFF
            did = _dungeon_id_or_ff(r.dungeon)
            ws = _world_state_mask(r.world_state_filter)
            out.append(f"  {{ {rid}, 0x{pid:04x}, 0x{did:02x}, 0x{ws:02x} }},  // {r.id}")
        out.append("};")
    else:
        out.append("const RandoRegionDef kRandoRegions[1] = { {0, 0xFFFF, 0xFF, 0} };  // placeholder")
    out.append(f"const uint32 kRandoRegionsCount = {len(region_list)};")
    # add-rando-ow-warp-shuffle — substrate consts + runtime tables.
    # kRandoOwRegionsFirst: first warp-region index (contiguous trailing
    # suffix, asserted in main()); kRandoNonWarpEdgesCount: the reachability
    # edge-scan bound when no warp axis is active. Present/digest gate
    # generation + activation fail-closed (empty-graph builds refuse warp
    # seeds; drifted graphs refuse certified slots).
    _ow = ow_meta or {"present": 0, "digest": 0, "flute": [],
                      "whirlpools": [], "vanilla_flute": [],
                      "hub_region": None}
    out.append(f"const uint32 kRandoOwRegionsFirst = {ow_regions_first};")
    out.append(f"const uint32 kRandoNonWarpEdgesCount = {ow_non_warp_edges};")
    out.append(f"const uint8 kRandoOwGraphPresent = {_ow['present']};")
    out.append(f"const uint32 kRandoOwGraphDigest = 0x{_ow['digest']:08x}u;")
    _hub = region_index.get(_ow["hub_region"], 0xFFFF) \
        if _ow.get("hub_region") else 0xFFFF
    out.append(f"const uint16 kRandoOwFluteHubRegion = 0x{_hub:04x};")
    _fl = _ow["flute"]
    if _fl:
        _scr_idx = {e["screen"]: i for i, e in enumerate(_fl)}
        out.append(f"const RandoOwFluteCandidate kRandoOwFluteCandidates[{len(_fl)}] = {{")
        for e in _fl:
            ridx = region_index[e["region_id"]]
            out.append(
                "  { 0x%02x, 0x%04x, %d, %d, 0x%04x, 0x%04x, 0x%04x, 0x%04x, "
                "0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, "
                "0x%04x },  // %s" % (
                    e["screen"], ridx, 1 if e["forced"] else 0,
                    e["slot"], e["vram"], e["bg_y"], e["bg_x"], e["link_y"],
                    e["link_x"], e["cam_y"], e["cam_x"], e["unk1"], e["unk2"],
                    e["icon_y"], e["icon_x"], e["sector"], e["component"]))
        out.append("};")
        out.append(f"const uint32 kRandoOwFluteCandidatesCount = {len(_fl)};")
        # Balanced-mode pairwise conflict bitmask (upstream getIgnored port):
        # row i bit j set = candidates i and j may not both host spots.
        _words = (len(_fl) + 31) // 32
        out.append(f"const uint32 kRandoOwFluteConflicts[{len(_fl)}][{_words}] = {{")
        for e in _fl:
            bits = [0] * _words
            for cs in e.get("conflicts", []):
                j = _scr_idx.get(cs)
                if j is None:
                    sys.exit(f"ow-graph: conflict screen 0x{cs:02x} of "
                             f"candidate 0x{e['screen']:02x} is not a "
                             f"candidate")
                bits[j >> 5] |= (1 << (j & 31))
            out.append("  { %s },  // 0x%02x" % (
                ", ".join(f"0x{b:08x}u" for b in bits), e["screen"]))
        out.append("};")
        out.append(f"const uint32 kRandoOwFluteConflictWords = {_words};")
    else:
        out.append("const RandoOwFluteCandidate kRandoOwFluteCandidates[1] = "
                   "{ {0,0xFFFF,0,0,0,0,0,0,0,0,0,0,0,0,0,0} };  // absent")
        out.append("const uint32 kRandoOwFluteCandidatesCount = 0;")
        out.append("const uint32 kRandoOwFluteConflicts[1][2] = { {0, 0} };  // absent")
        out.append("const uint32 kRandoOwFluteConflictWords = 2;")
    _vf = _ow["vanilla_flute"]
    if _vf:
        out.append("const uint8 kRandoOwVanillaFluteScreens[8] = { %s };" %
                   ", ".join(f"0x{s:02x}" for s in _vf))
    else:
        out.append("const uint8 kRandoOwVanillaFluteScreens[8] = {0};  // absent")
    _wp = _ow["whirlpools"]
    if _wp:
        # One row per whirlpool SIDE (2 per pair): the runtime keys landings
        # by individual whirlpool (kWhirlpoolAreas identity), so both sides
        # need rows — a pairs-only table left the B sides unindexable (the
        # OwWarp selfcheck caught exactly that).
        _sides = []
        for w0 in _wp:
            _sides.append((w0["screen"], w0["partner_screen"],
                           region_index[w0["region_id"]],
                           region_index[w0["partner_region_id"]],
                           w0["name"], w0["partner_name"]))
            _sides.append((w0["partner_screen"], w0["screen"],
                           region_index[w0["partner_region_id"]],
                           region_index[w0["region_id"]],
                           w0["partner_name"], w0["name"]))
        _sides.sort()
        out.append(f"const RandoOwWhirlpool kRandoOwWhirlpools[{len(_sides)}] = {{")
        for s0, ps, r0, pr, n0, pn in _sides:
            out.append("  { 0x%02x, 0x%02x, 0x%04x, 0x%04x },  // %s -> %s" % (
                s0, ps, r0, pr, n0, pn))
        out.append("};")
        out.append(f"const uint32 kRandoOwWhirlpoolsCount = {len(_sides)};")
    else:
        out.append("const RandoOwWhirlpool kRandoOwWhirlpools[1] = "
                   "{ {0,0,0xFFFF,0xFFFF} };  // absent")
        out.append("const uint32 kRandoOwWhirlpoolsCount = 0;")
    out.append("")
    # EdgeDef table — typedef lives in rando_logic.h.
    out.append("// Edge table — one row per logic.yaml `edges:` entry.")
    out.append("// Type definition: rando_logic.h::RandoEdgeDef.")
    if edges:
        out.append(f"const RandoEdgeDef kRandoEdges[{len(edges)}] = {{")
        for i, e in enumerate(edges):
            from_idx = region_index.get(e.from_, 0xFFFF)
            to_idx = region_index.get(e.to, 0xFFFF)
            poff, plen = edge_offsets[i]
            out.append(f"  {{ 0x{from_idx:04x}, 0x{to_idx:04x}, {poff}u, {plen}, {1 if e.one_way else 0}, 0 }},  // {e.from_} -> {e.to}")
        out.append("};")
    else:
        out.append("const RandoEdgeDef kRandoEdges[1] = { {0xFFFF, 0xFFFF, 0, 0, 0, 0} };  // placeholder")
    out.append(f"const uint32 kRandoEdgesCount = {len(edges)};")
    out.append("")

    # ----- Start region per world-state -----
    out.append("// Start region per world_state. Indexed by WorldState enum:")
    out.append("// Open=0, Standard=1, Inverted=2, Retro=3.")
    # Pinned mapping (Phase A): Open/Standard/Retro start in LinksHouse;
    # Inverted starts in LinksHouse_Inverted. Falls back to 0xFFFF if not
    # declared in logic.yaml — caller treats as "empty graph".
    start_region_names = {
        0: "LinksHouse",            # Open
        1: "LinksHouse",            # Standard
        # Phase B Slice 2 §50 — Inverted starts at LinksHouse_Inverted,
        # declared in `inverted/DarkWorld/South.yaml`. A trivial edge
        # LinksHouse_Inverted → DarkWorld_South provides the overworld
        # entry. Earlier Phase A1 stopgap of DarkWorld_South as the start
        # region is replaced — the new structure mirrors Standard's
        # LinksHouse → LightWorld_South split and reserves room for
        # future Inverted entrance shuffles (Phase C).
        2: "LinksHouse_Inverted",
        3: "LinksHouse",            # Retro
    }
    starts = []
    for ws in [0, 1, 2, 3]:
        nm = start_region_names[ws]
        if nm in region_index:
            starts.append(region_index[nm])
        else:
            starts.append(0xFFFF)
    start_csv = ", ".join(f"0x{s:04x}" for s in starts)
    out.append(f"const uint16 kRandoStartRegionByWorldState[4] = {{ {start_csv} }};")
    out.append("")

    # ----- Region-name lookup table for Rando_FindRegionByName -----
    out.append("// Region-name → region-id lookup. Linear scan; only used by")
    out.append("// authoring-time helpers (Rando_FindRegionByName).")
    out.append("typedef struct RandoRegionNameEntry {")
    out.append("  const char *name;")
    out.append("  uint16 id;")
    out.append("} RandoRegionNameEntry;")
    out.append("")
    if region_ids:
        out.append(f"static const RandoRegionNameEntry kRandoRegionNames[{len(region_ids)}] = {{")
        for rid in region_ids:
            idx = region_index[rid]
            out.append(f"  {{ \"{rid}\", {idx} }},")
        out.append("};")
        out.append(f"static const uint32 kRandoRegionNamesCount = {len(region_ids)};")
    else:
        out.append("static const RandoRegionNameEntry kRandoRegionNames[1] = { {\"\", 0} };")
        out.append("static const uint32 kRandoRegionNamesCount = 0;")
    out.append("")
    out.append("uint16 Rando_FindRegionByName(const char *name) {")
    out.append("  if (name == NULL) return 0xFFFF;")
    out.append("  for (uint32 i = 0; i < kRandoRegionNamesCount; i++) {")
    out.append("    const char *a = kRandoRegionNames[i].name;")
    out.append("    const char *b = name;")
    out.append("    while (*a && *a == *b) { a++; b++; }")
    out.append("    if (*a == 0 && *b == 0) return kRandoRegionNames[i].id;")
    out.append("  }")
    out.append("  return 0xFFFF;")
    out.append("}")
    out.append("")
    # Reverse lookup: region_id → name. Used by text spoiler region grouping.
    out.append("const char *Rando_GetRegionName(uint16 region_id) {")
    out.append("  for (uint32 i = 0; i < kRandoRegionNamesCount; i++) {")
    out.append("    if (kRandoRegionNames[i].id == region_id) return kRandoRegionNames[i].name;")
    out.append("  }")
    out.append("  return \"(unbound)\";")
    out.append("}")
    out.append("")
    # Location name table — used by text spoiler for human-readable rows.
    # Indexed parallel to kRandoLocations (sorted by id).
    if locations:
        out.append(f"static const char *kRandoLocationNames[{len(locations)}] = {{")
        for loc in sorted(locations.values(), key=lambda l: l.id):
            safe = loc.name.replace('"', '\\"')
            out.append(f"  \"{safe}\",  // id {loc.id}")
        out.append("};")
        out.append(f"static const uint32 kRandoLocationNamesCount = {len(locations)};")
    else:
        out.append("static const char *kRandoLocationNames[1] = { \"\" };")
        out.append("static const uint32 kRandoLocationNamesCount = 0;")
    out.append("")
    out.append("const char *Rando_GetLocationName(uint16 location_id) {")
    out.append("  for (uint32 i = 0; i < kRandoLocationsCount; i++) {")
    out.append("    if (kRandoLocations[i].id == location_id) {")
    out.append("      if (i < kRandoLocationNamesCount) return kRandoLocationNames[i];")
    out.append("      return \"(unnamed)\";")
    out.append("    }")
    out.append("  }")
    out.append("  return \"(unknown)\";")
    out.append("}")
    out.append("")

    # Compatibility receive-code accessor, now derived from the same semantic
    # metadata as grant planning. This intentionally returns 0xFF for ALTTPR
    # bytes whose semantics are direct in this port (0x51/0x52/0xAF), instead of
    # exposing an out-of-range code to the 76-entry vanilla receive tables.
    if items:
        max_item_id = max(i.id for i in items.values())
        out.append("uint8 Rando_VanillaItemForRegistryId(uint16 registry_item_id) {")
        out.append("  if (registry_item_id >= kRandoItemGrantMetadataCount) return 0xFF;")
        out.append("  const RandoItemGrantMetadata *m = &kRandoItemGrantMetadata[registry_item_id];")
        out.append("  return m->opcode == kRandoGrantOp_Receive ? (uint8)m->payload : 0xFF;")
        out.append("}")
        out.append("")

        # Item-name table — used by the spoiler writer for human-readable rows.
        # Indexed by registry item_id.
        name_by_id = [None] * (max_item_id + 1)
        for it in items.values():
            name_by_id[it.id] = it.display_name
        out.append("// Item registry ID → human-readable name. Used by the spoiler writer.")
        out.append(f"static const char *kRandoItemNames[{max_item_id + 1}] = {{")
        for i, nm in enumerate(name_by_id):
            safe = (nm or f"item_{i}").replace('"', '\\"')
            out.append(f"  \"{safe}\",  // id {i}")
        out.append("};")
        out.append(f"static const uint16 kRandoItemNamesCount = {max_item_id + 1};")
        out.append("")
        out.append("const char *Rando_GetItemName(uint16 item_id) {")
        out.append("  if (item_id >= kRandoItemNamesCount) return \"(unknown)\";")
        out.append("  return kRandoItemNames[item_id];")
        out.append("}")
        out.append("")

    # ----- Phase B Slice 2 — per-world-state predicate overrides -----
    # Emit one array per non-default world state that has overrides. The
    # array is sorted by location_id for binary-search lookup at runtime.
    # An empty/no-override world state emits a 1-entry placeholder so the
    # extern symbol exists for the C ABI.
    out.append("// Phase B Slice 2 — per-world-state location predicate overrides.")
    out.append("// Each non-Standard world state with Inverted-style overrides emits its own")
    out.append("// table. Sorted by location_id for binary-search lookup.")
    out.append("// Empty tables retain a 1-entry placeholder so the symbol always exists.")
    out.append("")
    for ws_id, ws_name in [(2, "Inverted"), (3, "Retro")]:
        entries = override_offsets.get(ws_id, [])
        entries_sorted = sorted(entries, key=lambda t: t[0])
        if entries_sorted:
            out.append(f"const RandoLocationPredOverride kRandoLocationPredOverrides_{ws_name}[{len(entries_sorted)}] = {{")
            for loc_id, region_override, (cr_o, cr_l), (cp_o, cp_l), (aa_o, aa_l) in entries_sorted:
                out.append(
                    "  { %d, 0x%04x, %d, %d, %d, %d, %d, %d }, " %
                    (loc_id, region_override, cr_o, cr_l, cp_o, cp_l, aa_o, aa_l)
                )
            out.append("};")
            out.append(f"const uint32 kRandoLocationPredOverrides_{ws_name}Count = {len(entries_sorted)};")
        else:
            # emit 0xFFFF in the region_override slot so the
            # placeholder matches the "no region change" sentinel rather
            # than accidentally binding loc 0 to region 0 when a future
            # entry is added without auditing the placeholder.
            out.append(f"const RandoLocationPredOverride kRandoLocationPredOverrides_{ws_name}[1] = {{ {{0, 0xFFFF, 0, 0, 0, 0, 0, 0}} }};")
            out.append(f"const uint32 kRandoLocationPredOverrides_{ws_name}Count = 0;")
        out.append("")

    # Per-world-state edges (Inverted-only entries).
    out.append("// Phase B Slice 2 — per-world-state edges (Inverted overworld + dungeon entries).")
    for ws_id, ws_name in [(2, "Inverted")]:
        entries = ws_edge_offsets.get(ws_id, [])
        if entries:
            out.append(f"const RandoEdgeDef kRandoEdges_{ws_name}[{len(entries)}] = {{")
            for from_idx, to_idx, one_way, (off, length) in entries:
                out.append(
                    "  { %d, %d, %d, %d, %d, 0 }, " %
                    (from_idx, to_idx, off, length, one_way)
                )
            out.append("};")
            out.append(f"const uint32 kRandoEdges_{ws_name}Count = {len(entries)};")
        else:
            out.append(f"const RandoEdgeDef kRandoEdges_{ws_name}[1] = {{ {{0, 0, 0, 0, 0, 0}} }};")
            out.append(f"const uint32 kRandoEdges_{ws_name}Count = 0;")
        out.append("")

    # -----------------------------------------------------------------------
    # §12.6 — ROM-version verification status tables. The runtime consults
    # these so the spoiler can warn when a seed enables a trick / glitch level
    # not yet confirmed to behave on the US 1.0 ROM. Struct + enum are declared
    # in rando_logic.h.
    # -----------------------------------------------------------------------
    tr = trick_status_rows or []
    out.append("// §12.6 — ROM-version verification status (op_registry.yaml tricks/glitch_levels).")
    out.append(f"const RandoTrickStatus kRandoTrickStatus[{max(len(tr), 1)}] = {{")
    if tr:
        for r in tr:
            out.append('  { %d, %d, "%s" },' % (r["bit"], r["status_int"], r["id"]))
    else:
        out.append('  { 0, 0, "" },')
    out.append("};")
    out.append(f"const uint32 kRandoTrickStatusCount = {len(tr)};")
    out.append("")
    gl = glitch_status_rows or []
    out.append(f"const RandoGlitchLevelStatus kRandoGlitchLevelStatus[{max(len(gl), 1)}] = {{")
    if gl:
        for r in gl:
            out.append('  { %d, %d, "%s" },' % (r["level"], r["status_int"], r["id"]))
    else:
        out.append('  { 0, 0, "" },')
    out.append("};")
    out.append(f"const uint32 kRandoGlitchLevelStatusCount = {len(gl)};")
    out.append("")

    atomic_write_text(path, "\n".join(out) + "\n")


def _location_type_id(t: str) -> int:
    # APPEND-ONLY: existing indices 0-13 are baked into kRandoLocations bytes
    # in src/rando/logic_data.c. Adding new types past 13 preserves them.
    types = ["Chest", "BigChest", "Npc", "Standing", "Pedestal", "Dash", "Dig", "Drop",
             "Fountain", "Trade", "Prize_Crystal", "Prize_Pendant", "Prize_Event", "Medallion",
             "Shop",        # 14 — Phase B Slice 3a Retro shop purchase slot
             "ShopUpgrade", # 15 — Phase B Slice 3a Capacity Upgrade (identity-placed)
             "TakeAny",     # 16 — Phase B Slice 3b Retro take-any cave slot (per-seed active subset)
             "Pot",         # 17 — add-rando-pot-sanity dungeon pot (per-tier active subset)
             "EnemyDrop",    # 18 — add-rando-enemy-drop-sanity forced enemy key drop
             "Enemy",        # 19 — add-rando-dungeon-enemy-checks ordinary dungeon enemy check
             "Grass",        # 20 — add-rando-grass-rock-shuffle bush/thick-grass check
             "Rock",         # 21 — add-rando-grass-rock-shuffle light/heavy rock check
             "Bonk"]         # 22 — add-rando-bonk-sanity placed OW bonk-item sprite check
    if t not in types:
        # HARD-FAIL (add-rando-bonk-sanity hardening): the old `return 0`
        # silently emitted unknown types as Chest — exactly how the first
        # Bonk build shipped rows invisible to every LOCTYPE_Bonk consumer.
        sys.exit(f"rando_logic_gen: unknown location type {t!r} "
                 f"(_location_type_id has no ordinal for it)")
    return types.index(t)


def _world_state_mask(wsf: list) -> int:
    if not wsf:
        return 0  # 0 = all world-states (the common case)
    mask = 0
    for ws in wsf:
        # Strict — typoed names like "retros" silently demoted to Open (bit 0)
        # under the prior `.get(ws, 0)` default, invalidating every Open digest
        # at once. Raise loudly instead.
        if ws not in WORLD_STATES:
            raise ParseError(
                f"unknown world_state {ws!r} in world_state_filter — "
                f"valid: {sorted(WORLD_STATES.keys())}"
            )
        mask |= 1 << WORLD_STATES[ws]
    return mask


def _dungeon_id_or_ff(name) -> int:
    if not name:
        return 0xFF
    try:
        return _resolve_dungeon_id(name)
    except ParseError:
        return 0xFF


# ---------------------------------------------------------------------------
# Main driver
# ---------------------------------------------------------------------------
def main(argv=None):
    p = argparse.ArgumentParser(description="Generate rando C data and headers")
    p.add_argument("--strict", action="store_true", help="Fail on any well-formedness warning")
    p.add_argument("--update-region-lock", action="store_true",
                   help="Rewrite assets/rando/region_ids.lock.json from the "
                        "current region set (add-rando-ow-warp-shuffle; only "
                        "after an INTENDED region-set change)")
    p.add_argument(
        "--update-hint-metadata-lock",
        action="store_true",
        help="Approve new Hints algorithm/text metadata fingerprints "
             "(each changed axis requires its matching version bump when "
             "replacing an existing lock)",
    )
    p.add_argument("--keep-hint-registry-contract", action="store_true",
                   help="For item-only metadata changes, validate and retain the "
                        "approved location contract (supports public base-only builds)")
    p.add_argument("--out-headers", default=str(RANDO_SRC), help="Destination for emitted headers (default: src/rando/)")
    p.add_argument("--out-data", default=str(RANDO_SRC), help="Destination for emitted logic_data.c (default: src/rando/)")
    p.add_argument("--logic", default=None, help="Path to logic.yaml (optional)")
    p.add_argument("--macros", default=None, help="Path to macros.yaml (optional)")
    args = p.parse_args(argv)

    ops_path = RANDO_ASSETS / "op_registry.yaml"
    items_path = RANDO_ASSETS / "item_registry.yaml"
    locs_path = RANDO_ASSETS / "location_registry.yaml"
    hint_metadata_path = RANDO_ASSETS / "hint_metadata.yaml"
    hint_metadata_lock_path = RANDO_ASSETS / "hint_metadata.lock.json"
    hint_registry_contract_path = (
        RANDO_ASSETS / "hint_registry_contract.json"
    )
    schema_path = RANDO_ASSETS / "logic.schema.yaml"
    macros_path = Path(args.macros) if args.macros else (RANDO_ASSETS / "macros.yaml")
    logic_path = Path(args.logic) if args.logic else (RANDO_ASSETS / "logic.yaml")

    ops = load_ops(ops_path)
    # Phase B §56 — populate the OP_TRICK operand lookup. The codegen now
    # resolves `OP_TRICK(boots-clip)` to the bit-position operand byte;
    # without this load, every trick reference would fail well-formedness.
    global TRICK_BITS, TRICK_STATUS
    TRICK_BITS = load_tricks(ops_path)
    # §12.6 — ROM-version verification status for tricks + glitch levels.
    trick_status_rows, TRICK_STATUS = load_trick_status(ops_path)
    glitch_status_rows = load_glitch_levels(ops_path)
    items = load_items(items_path)
    locations = load_locations(locs_path)
    contract_locations = dict(locations)
    if not schema_path.exists():
        print(f"WARNING: {schema_path} not found", file=sys.stderr)
    macros = load_macros(macros_path)
    (logic_regions, logic_edges, logic_loc_preds, logic_macros,
     world_state_overrides, world_state_edges) = load_logic(logic_path)
    contract_regions = dict(logic_regions)
    contract_region_ids = _region_ids_for_codegen(contract_regions)
    contract_world_state_overrides = {
        location: dict(world_states)
        for location, world_states in world_state_overrides.items()
    }

    # add-rando-pot-sanity: merge the generated local pot registry into BOTH the
    # location set (ids / kRandoLocations rows) and the logic binding (region_id +
    # can_reach), then emit pot_lookup.h. Pot LOCs grow kRandoLocationsCount but
    # stay OUT of placement until a tier selects them — rando_placement.c skips
    # LOCTYPE_Pot in the open-location + junk-pad loops (mirroring inactive
    # Take-Any), so pot-shuffle off is placement-byte-identical (design D9).
    pot_locs, pot_lookup_rows, door_pot_sources = load_pots(
        Path("assets/rando/pots.gen.yaml"), logic_regions)
    locations.update(pot_locs)
    logic_loc_preds.update(pot_locs)

    # add-rando-enemy-drop-sanity: merge the local forced enemy-key registry
    # into the location set when present. Assetless builds emit no EnemyDrop
    # locations and an empty lookup; rando_placement.c fails closed if the
    # effective setting asks for active enemy-drop checks without this registry.
    enemy_drops_path = Path("assets/rando/enemy_drops.gen.yaml")
    (enemy_drop_locs, enemy_drop_lookup_rows, door_enemy_drop_sources,
     enemy_key_depth_rows, enemy_hce_big_key_locations) = load_enemy_drops(
        enemy_drops_path, logic_regions)
    locations.update(enemy_drop_locs)
    logic_loc_preds.update(enemy_drop_locs)
    _apply_enemy_drop_key_terms(
        logic_loc_preds, world_state_overrides,
        enemy_key_depth_rows, enemy_hce_big_key_locations)
    enemy_drop_source_counts = (
        _enemy_source_counts(load_yaml(enemy_drops_path)) if enemy_drops_path.exists() else {}
    )

    # add-rando-dungeon-enemy-checks: merge ordinary dungeon enemy checks as a
    # distinct Enemy type. These are not vanilla key sources, so they do not
    # participate in enemy-drop source accounting; they only use the same
    # key-depth terms to avoid certifying enemies behind uncollected keys.
    enemy_checks_path = Path("assets/rando/enemy_checks.gen.yaml")
    enemy_check_locs, enemy_check_lookup_rows, door_enemy_check_sources = load_enemy_checks(
        enemy_checks_path,
        logic_regions,
        enemy_drop_source_counts,
    )
    locations.update(enemy_check_locs)
    logic_loc_preds.update(enemy_check_locs)

    # add-rando-grass-rock-shuffle: merge the generated local terrain registry
    # (overworld bushes / thick grass / light+heavy rocks) into the location
    # set + logic binding, then emit terrain_lookup.h below. Terrain LOCs grow
    # kRandoLocationsCount but stay OUT of placement until the grass/rock tiers
    # select them — rando_placement.c's terrain_active() is unconditionally
    # false in Phase 2, and the skip-triple (open-location loop, junk-pad
    # target, Placement_SelfCheck) all key on it, so every existing seed is
    # placement-byte-identical.
    terrain_locs, terrain_lookup_rows = load_terrain(
        Path("assets/rando/terrain.gen.yaml"), logic_regions)
    locations.update(terrain_locs)
    logic_loc_preds.update(terrain_locs)

    # add-rando-bonk-sanity — placed OW bonk-item sprite checks. Ids live in
    # the free 2634..3071 gap BELOW terrain so terrain stays the id-sorted
    # registry's contiguous suffix (the kRandoNonTerrainLocationsCount
    # reachability fast-path). Inert until bonk_shuffle selects them
    # (bonk_active(), same skip-triple discipline as terrain).
    bonk_locs, bonk_lookup_rows = load_bonk(
        Path("assets/rando/bonk.gen.yaml"), logic_regions)
    locations.update(bonk_locs)
    logic_loc_preds.update(bonk_locs)

    # add-rando-ow-warp-shuffle — OW screen-component substrate. Regions
    # merge LAST (load_ow_graph populated _OW_REGION_IDS, so
    # _region_ids_for_codegen appends them after the chain suffix — every
    # pre-existing id stays put, guarded by region_ids.lock.json below), and
    # the warp edges are appended at the TAIL of logic_edges so they form the
    # kRandoEdges suffix bounded by kRandoNonWarpEdgesCount when no warp axis
    # is active. Adds NO locations.
    ow_non_warp_edge_count = len(logic_edges)
    ow_regions, ow_edges, ow_meta = load_ow_graph(
        Path("assets/rando/ow_graph.gen.yaml"), logic_regions, logic_edges)
    for _rid, _r in ow_regions.items():
        if _rid in logic_regions:
            sys.exit(f"ow-graph: region id {_rid!r} collides with an "
                     f"existing logic region")
        logic_regions[_rid] = _r
    logic_edges.extend(ow_edges)
    # Quotient cross-check runs below, once all_errors exists (its findings
    # ride the standard soft-error channel; --strict fails on any).
    ow_pre_warp_edges = logic_edges[:ow_non_warp_edge_count]

    # Region-id lock: the committed snapshot of every NON-warp region's
    # name->id binding. Drift = renumbering = digest break; regenerate with
    # --update-region-lock after an INTENDED region-set change.
    _region_lock_path = Path("assets/rando/region_ids.lock.json")
    _all_rids = _region_ids_for_codegen(logic_regions)
    _non_warp_map = {rid: i for i, rid in enumerate(_all_rids)
                     if rid not in _OW_REGION_ID_SET}
    if "--update-region-lock" in sys.argv:
        with open(_region_lock_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(_non_warp_map, f, indent=1, sort_keys=True)
            f.write("\n")
        print(f"region-id lock updated: {_region_lock_path}")
    elif _region_lock_path.exists():
        with open(_region_lock_path, "r", encoding="utf-8") as f:
            _locked = json.load(f)
        if _locked != _non_warp_map:
            _drift = sorted(set(_locked.items()) ^ set(_non_warp_map.items()))
            sys.exit(f"region_ids.lock.json drift ({len(_drift)} entries, "
                     f"e.g. {_drift[:4]}). If the region-set change is "
                     f"intended, rerun with --update-region-lock and review "
                     f"the diff.")
    else:
        sys.exit(f"{_region_lock_path} missing — run with "
                 f"--update-region-lock once to create it.")

    # Edge-override budget (randomizer-logic spec): the runtime store
    # Rando_SetEntranceEdgeOverride silently ignores old_to_region ids at or
    # above kEntranceEdgeOverrideMax, so every override-ELIGIBLE region — each
    # dungeon's override key (interior/lobby region, falling back to the entry
    # region) — must keep an id below that cap. Parse the authoritative
    # kDungeons table and the cap from the C sources rather than duplicating
    # either list here; a violation is a hard codegen failure because the
    # runtime failure mode is a silently un-remapped dungeon door.
    _se_src = Path("src/rando/shuffle_entrance.c").read_text(encoding="utf-8")
    _rl_src = Path("src/rando/rando_logic.c").read_text(encoding="utf-8")
    _cap_m = re.search(r"#define\s+kEntranceEdgeOverrideMax\s+(\d+)", _rl_src)
    _tbl_m = re.search(r"kDungeons\[kEntranceDungeonCount\]\s*=\s*\{(.*?)\n\};",
                       _se_src, re.S)
    if not _cap_m or not _tbl_m:
        sys.exit("edge-override budget: cannot parse kEntranceEdgeOverrideMax "
                 "(rando_logic.c) or kDungeons (shuffle_entrance.c)")
    _ov_cap = int(_cap_m.group(1))
    _ov_bad = []
    for _row_m in re.finditer(r"\{\s*\"[^\"]+\"[^}]*\}", _tbl_m.group(1)):
        _strs = re.findall(r"\"([^\"]+)\"", _row_m.group(0))
        if len(_strs) < 2:
            continue
        _ov_key = _strs[2] if len(_strs) >= 3 else _strs[1]
        _ov_id = _non_warp_map.get(_ov_key)
        if _ov_id is None:
            sys.exit(f"edge-override budget: dungeon override key {_ov_key!r} "
                     f"(shuffle_entrance.c kDungeons) is not a known logic "
                     f"region")
        if _ov_id >= _ov_cap:
            _ov_bad.append((_ov_key, _ov_id))
    if _ov_bad:
        sys.exit(f"edge-override budget: dungeon override-key region id(s) at "
                 f"or above kEntranceEdgeOverrideMax={_ov_cap}: {_ov_bad} — "
                 f"keep these regions early in id order or raise the cap in "
                 f"rando_logic.c")

    # Region budget: the reachability walker's structures are sized by
    # kReachabilityMaxRegions (rando_logic.c) = 512 after this change;
    # regions at or above the cap are silently never walked, so the budget
    # is a hard codegen error, not a warning.
    # Keep in sync with rando_logic.h. The budget is cap - 1: the TOP id is
    # the reserved entrance cross-void sink (kCrossVoidRegion) and may never
    # be a real region.
    _REACHABILITY_MAX_REGIONS = 512 - 1
    if len(_all_rids) > _REACHABILITY_MAX_REGIONS:
        sys.exit(f"region budget exceeded: {len(_all_rids)} regions > "
                 f"kReachabilityMaxRegions ({_REACHABILITY_MAX_REGIONS}); "
                 f"non-warp {len(_non_warp_map)}, warp "
                 f"{len(_OW_REGION_IDS)}")
    # Warp regions must be the contiguous trailing suffix (inertness).
    _ow_first = len(_all_rids) - len(_OW_REGION_IDS)
    if _all_rids[_ow_first:] != _OW_REGION_IDS:
        sys.exit("ow-graph: warp regions are not the trailing region-id "
                 "suffix — _region_ids_for_codegen ordering regressed")

    # Registry-wide duplicate-id hard-fail. Every generated family allocates
    # ids from its own base block, and a block overlap silently corrupts every
    # id-keyed consumer (reachability bitsets, placement digests, dispatch
    # lookups) — the terrain base collided with enemy-check ids >= 2048 exactly
    # this way, and only corpus digest drift caught it. Fail the BUILD instead.
    _ids_seen = {}
    for _loc in locations.values():
        if _loc.id in _ids_seen:
            sys.exit(f"rando_logic_gen: DUPLICATE location id {_loc.id}: "
                     f"{_ids_seen[_loc.id]!r} vs {_loc.name!r} — "
                     f"family base blocks overlap; move the newer family's base")
        _ids_seen[_loc.id] = _loc.name
    del _ids_seen

    # add-enemy-souls (task 4.4): AND the kill-gated rooms' resident-soul
    # requirements into every predicate those rooms gate (fork chests/bosses/
    # prizes by id, the Aga1 event by name, generated enemy/pot locations by
    # door-region/room). Terms are NeedsEnemySoul(...) macro text, inert below
    # souls_shuffle=all, so off/bosses placements stay byte-identical. Runs
    # BEFORE world-state override compilation and the door-shuffle wrap (the
    # enemies tier degrades to Bosses under door shuffle, so the door arm never
    # needs these terms). Absent table -> no wraps + kRandoSoulRoomsBaked=0
    # (BuildItemPool fails closed for souls_shuffle=all).
    soul_rooms = load_soul_rooms(Path("assets/rando/soul_rooms.gen.yaml"))
    _apply_soul_room_wraps(
        soul_rooms, locations, logic_loc_preds, world_state_overrides,
        enemy_drop_locs, enemy_drop_lookup_rows, enemy_drops_path,
        enemy_check_locs, enemy_checks_path,
        pot_locs, Path("assets/rando/pots.gen.yaml"),
        logic_edges=logic_edges)
    souls_baked, *_unused, soul_pin_rooms = soul_rooms

    # add-npc-souls — NPC-check soul gates + the Kiki PoD entry-edge gate.
    # Terms are NeedsNpcSoul(...) macro text, inert at npc_souls=off, so
    # default placements stay byte-identical.
    _apply_npc_soul_gates(Path("assets/rando/npc_souls.yaml"),
                          logic_loc_preds, world_state_overrides, logic_edges)

    # Under Inverted, Link starts in `LinksHouse_Inverted`, and base
    # `LinksHouse` has NO in-edges (Logic_SelfCheck asserts it stays
    # unreachable there — region scoping is by graph topology). The room-0x104
    # pots are region-bound to base LinksHouse via pot_logic_overrides.yaml,
    # yet the runtime Inverted start house IS room 0x104 — sphere-0 collectible
    # but phantom-unreachable in logic. Rebind them with a per-world-state
    # override, the same mechanism as the Link's Uncle rebind in
    # logic_parts/inverted/HyruleCastleEscape.yaml. Synthesized here rather
    # than authored in a committed YAML because pot names only exist when the
    # gitignored pots.gen.yaml is present (a name-keyed YAML entry would
    # hard-error assetless codegen runs). Runs AFTER every predicate wrap so
    # the override copy carries the pot's final can_reach.
    for _pot in pot_locs.values():
        if _pot.region == "LinksHouse":
            world_state_overrides.setdefault(_pot.name, {})[kWorldState_Inverted] = \
                dataclasses.replace(_pot, region="LinksHouse_Inverted")

    # Merge macros from macros.yaml and logic.yaml (logic.yaml takes precedence).
    all_macros = {**macros, **logic_macros}

    # Parse macro bodies into ASTs (resolve calls / nested macros after all macros are loaded).
    parsed_macro_bodies = {}
    macro_errors = []
    for name, m in all_macros.items():
        try:
            ast = parse_predicate(m.body)
            ast = resolve_calls(ast, ops, all_macros)
            parsed_macro_bodies[name] = ast
        except ParseError as e:
            macro_errors.append(f"macro {name}: parse error: {e}")

    if macro_errors:
        for err in macro_errors:
            print(f"ERROR: {err}", file=sys.stderr)
        if args.strict:
            sys.exit(1)

    # ----- Well-formedness checks (task 3.10) -----
    all_errors = []

    # add-rando-ow-warp-shuffle — quotient cross-check (design D1): findings
    # join the soft-error channel so --strict fails on any unreconciled pair.
    ow_quotient_check(ow_meta, ow_pre_warp_edges, locations,
                      Path("assets/rando/ow_quotient_allowlist.yaml"),
                      all_errors)

    ids_to_names: dict[int, list[str]] = {}
    for loc in locations.values():
        ids_to_names.setdefault(loc.id, []).append(loc.name)
    for loc_id, names in sorted(ids_to_names.items()):
        if len(names) > 1:
            all_errors.append(
                f"location id {loc_id} is assigned to multiple locations: "
                f"{', '.join(repr(n) for n in names)}"
            )

    # 1. Detect logic.yaml location overrides that don't match any registry entry —
    #    these would be silently dropped without this check, masking translation
    #    typos (e.g., underscored vs spaced names). Per the agent-discovered
    #    silent-drop bug.
    registry_loc_names = set(locations.keys())
    for override_id in logic_loc_preds.keys():
        if override_id not in registry_loc_names:
            all_errors.append(
                f"logic.yaml location override id={override_id!r} does not "
                f"match any entry in assets/rando/location_registry.yaml — "
                f"this predicate override is being dropped silently."
            )

    # 2. Detect orphan regions (parent references that don't resolve).
    if logic_regions:
        region_ids = set(logic_regions.keys())
        for r in logic_regions.values():
            if r.parent and r.parent not in region_ids:
                all_errors.append(
                    f"region {r.id!r} declares parent {r.parent!r} which is not "
                    f"a known region in logic.yaml."
                )

    # 3. Detect edges referencing unknown regions.
    if logic_regions:
        region_ids = set(logic_regions.keys())
        for i, e in enumerate(logic_edges):
            if e.from_ not in region_ids:
                all_errors.append(
                    f"edge[{i}] {e.from_!r} -> {e.to!r}: 'from' region not declared."
                )
            if e.to not in region_ids:
                all_errors.append(
                    f"edge[{i}] {e.from_!r} -> {e.to!r}: 'to' region not declared."
                )

    # 4. Warn on locations in logic.yaml that don't carry any explicit predicate
    #    (currently impossible — the loader fills defaults — but documents intent).
    #    Skipped: every loaded location has all three predicate fields.

    # 5. Detect Phase A logic.yaml location overrides that reference a logic.yaml
    #    region not in the logic.yaml regions list.
    if logic_regions:
        region_ids = set(logic_regions.keys())
        for raw_id, override in logic_loc_preds.items():
            if override.region and override.region not in region_ids:
                all_errors.append(
                    f"logic.yaml location {raw_id!r} declares region {override.region!r} "
                    f"which is not in logic.yaml regions."
                )

    # 6. Silent-region-bypass guard. A location with no `region:` field
    #    in logic.yaml or logic_parts/* gets encoded with region_id=0xFFFF,
    #    which makes it bypass region-membership reachability entirely
    #    (the predicate VM treats it as "always-reachable region"). That
    #    is CORRECT for the allowlisted types — Medallion is a generation-
    #    time config slot (not a chest), Shop locations are gated by
    #    world-state filter rather than region, CapacityUpgrade follows
    #    Shop. For ordinary chests/NPCs/standing-items it is WRONG: the
    #    location becomes reachable from sphere 0 regardless of any
    #    entry-edge predicate (e.g., losing the Standard-mode RescuedZelda
    #    gate on the LightWorld_NorthEast entry edge).
    #
    #    This bug pattern landed once when removing the
    #    logic_parts/45_lightworld_northeast.yaml duplicate-override
    #    silently dropped the region binding on King Zora et al.
    #    Warn at codegen time so the next such regression
    #    fails the build instead of being caught by playtest.
    # Types that don't require a `region:` binding because their reachability
    # gating happens via a different mechanism:
    #   - Medallion: generation-time config slot (medallion-shuffle), not a
    #     pool placement; reachability isn't checked.
    #   - Shop: gated by world_state_filter=[retro], not by region predicate.
    #   - ShopUpgrade: same as Shop (capacity-upgrade slots in Retro mode).
    #   - TakeAny: Phase B Slice 3b Retro take-any cave slots. Like Shop, these
    #     are gated by world_state_filter=[retro] and an additional per-seed
    #     "active" gate in the placer (only 9 of 62 slots emit per seed). They
    #     are reward-pinned by role, never assumed-fill targets, so region
    #     reachability isn't used to place into them. (Consistent with the 3a
    #     Shop precedent; see add-rando-retro-takeany/design.md §D4, §5.)
    #   - Prize_Event: game-event-style logic-affecting sites (Zelda rescue,
    #     Agahnim 1/2, Ganon, Bomb Merchant). Most carry a region binding in
    #     logic.yaml or logic_parts, but Bomb Merchant — Inverted-only and
    #     deferred to Slice 2 — does not yet; allowlisting Prize_Event lets
    #     the Bomb Merchant warning stay quiet until Inverted logic_parts
    #     land and its DarkWorld_South region binding is wired.
    # add-rando-shopsanity removed "Shop": the 27 regular shop slots are real
    # fill locations with logic_parts/50_shops.yaml region bindings, so a
    # region-less Shop row is now a codegen error (ShopUpgrade/TakeAny stay
    # config-slot-style and region-optional).
    REGION_OPTIONAL_TYPES = {"Medallion", "ShopUpgrade", "TakeAny", "Prize_Event"}
    if logic_regions:
        region_ids = set(logic_regions.keys())
        for loc_name, loc in locations.items():
            override = logic_loc_preds.get(loc.name) or logic_loc_preds.get(loc_name)
            has_region = bool(override and override.region and override.region in region_ids)
            if not has_region and loc.type not in REGION_OPTIONAL_TYPES:
                all_errors.append(
                    f"location {loc.name!r} (type {loc.type}) has no `region:` "
                    f"binding — encoded as region_id=0xFFFF which bypasses "
                    f"region-membership reachability. Either bind a region in "
                    f"logic.yaml / logic_parts, or add `{loc.type}` to "
                    f"REGION_OPTIONAL_TYPES if this is a config-slot-style "
                    f"location that genuinely doesn't have a region."
                )

    # Compile location predicates (only those that have overrides in logic.yaml).
    location_predicates = {}
    for loc_name, loc in locations.items():
        # logic.yaml's predicate overrides take precedence; default to TRUE() / FALSE() otherwise.
        overrides = logic_loc_preds.get(loc.name) or logic_loc_preds.get(loc_name)
        cr_src = overrides.can_reach if overrides else "TRUE()"
        cp_src = overrides.can_place if overrides else "TRUE()"
        aa_src = overrides.always_allow if overrides else "FALSE()"
        encoded = {}
        for label, src in [("can_reach", cr_src), ("can_place", cp_src), ("always_allow", aa_src)]:
            try:
                ast = parse_predicate(src)
                ast = resolve_calls(ast, ops, all_macros)
                ast = expand_macros(ast, all_macros, parsed_macro_bodies, ops)
                errs = well_formedness(ast, ops, items, logic_regions, locations, label)
                if errs:
                    for e in errs:
                        all_errors.append(f"location {loc_name}.{label}: {e}")
                encoded[label] = encode_predicate(ast, ops, items, logic_regions, locations)
            except ParseError as e:
                all_errors.append(f"location {loc_name}.{label}: parse error: {e}")
                encoded[label] = b"\x0d\x00"  # safe default: FALSE
        location_predicates[loc_name] = encoded

    # Compile edge predicates.
    edge_predicates = []
    for i, e in enumerate(logic_edges):
        try:
            ast = parse_predicate(e.predicate)
            ast = resolve_calls(ast, ops, all_macros)
            ast = expand_macros(ast, all_macros, parsed_macro_bodies, ops)
            errs = well_formedness(ast, ops, items, logic_regions, locations, f"edge[{i}]")
            if errs:
                for err in errs:
                    all_errors.append(f"edge[{i}] {e.from_}->{e.to}: {err}")
            edge_predicates.append(encode_predicate(ast, ops, items, logic_regions, locations))
        except ParseError as ex:
            all_errors.append(f"edge[{i}]: parse error: {ex}")
            edge_predicates.append(b"\x0d\x00")  # safe default: FALSE

    # Phase B Slice 2 — compile per-world-state location predicate overrides.
    # Output shape: world_state_id → list[ (loc_name, region_str_or_None, encoded_dict) ]
    # Each loc_name MUST be present in `locations` (registry); unknown names
    # are dropped with a WARN (same well-formedness contract as the base path).
    # `region_str_or_None` is the override's region (from the world-state YAML);
    # None means "no region change vs the base" — the codegen emits 0xFFFF.
    compiled_overrides: dict[int, list[tuple[str, str | None, dict[str, bytes]]]] = {}
    for loc_name_or_id, ws_map in world_state_overrides.items():
        # Resolve the override key to a registry location name. Override
        # YAMLs key by location id (e.g., "Eastern Palace - Compass Chest");
        # the registry uses the same canonical names.
        if loc_name_or_id not in locations:
            all_errors.append(
                f"world-state override for {loc_name_or_id!r} does not match "
                f"a registry entry — drop or fix the location id."
            )
            continue
        for ws_id, override_def in ws_map.items():
            encoded = {}
            for label, src in [
                ("can_reach", override_def.can_reach),
                ("can_place", override_def.can_place),
                ("always_allow", override_def.always_allow),
            ]:
                try:
                    ast = parse_predicate(src)
                    ast = resolve_calls(ast, ops, all_macros)
                    ast = expand_macros(ast, all_macros, parsed_macro_bodies, ops)
                    errs = well_formedness(ast, ops, items, logic_regions, locations, label)
                    if errs:
                        for e in errs:
                            all_errors.append(
                                f"override(ws={ws_id}) {loc_name_or_id}.{label}: {e}"
                            )
                    encoded[label] = encode_predicate(ast, ops, items, logic_regions, locations)
                except ParseError as e:
                    all_errors.append(
                        f"override(ws={ws_id}) {loc_name_or_id}.{label}: parse error: {e}"
                    )
                    encoded[label] = b"\x0d\x00"  # safe default: FALSE
            # Capture the override's region if it differs from the empty
            # default. Codegen at emit time resolves the region name to
            # an index into the stable region-id order; if the name doesn't match
            # any region OR equals the base region, region_override is set
            # to 0xFFFF.
            region_str = override_def.region if override_def.region else None
            compiled_overrides.setdefault(ws_id, []).append((loc_name_or_id, region_str, encoded))

    # Compile per-world-state edge predicates (Inverted overworld + cross-
    # region traversal). Encode and bucket by ws_id; emitted alongside base
    # edges in logic_data.c.
    compiled_world_state_edges: dict[int, list[tuple[EdgeDef, bytes]]] = {}
    for ws_id, edges_list in world_state_edges.items():
        for e in edges_list:
            try:
                ast = parse_predicate(e.predicate)
                ast = resolve_calls(ast, ops, all_macros)
                ast = expand_macros(ast, all_macros, parsed_macro_bodies, ops)
                errs = well_formedness(ast, ops, items, logic_regions, locations,
                                       f"ws-edge[{ws_id}]")
                if errs:
                    for err in errs:
                        all_errors.append(
                            f"ws-edge(ws={ws_id}) {e.from_}->{e.to}: {err}"
                        )
                compiled_world_state_edges.setdefault(ws_id, []).append(
                    (e, encode_predicate(ast, ops, items, logic_regions, locations))
                )
            except ParseError as ex:
                all_errors.append(
                    f"ws-edge(ws={ws_id}): parse error: {ex}"
                )
                compiled_world_state_edges.setdefault(ws_id, []).append(
                    (e, b"\x0d\x00")
                )

    # Boss-shuffle runtime — compile the per-boss kill predicates for
    # OP_CAN_KILL_BOSS dispatch. Same parse→resolve→expand→encode pipeline as
    # locations; each body reuses a canonical CanKill<Boss> macro so the
    # off-shuffle resolution is byte-identical to the inline predicate. The list
    # order is the boss-pool-index contract (see BOSS_KILL_BODIES).
    boss_kill_predicates = []
    for bi, body in enumerate(BOSS_KILL_BODIES):
        try:
            ast = parse_predicate(body)
            ast = resolve_calls(ast, ops, all_macros)
            ast = expand_macros(ast, all_macros, parsed_macro_bodies, ops)
            errs = well_formedness(ast, ops, items, logic_regions, locations,
                                   f"boss_kill[{bi}]")
            if errs:
                for e in errs:
                    all_errors.append(f"boss_kill[{bi}] ({body}): {e}")
            boss_kill_predicates.append(encode_predicate(ast, ops, items, logic_regions, locations))
        except ParseError as e:
            all_errors.append(f"boss_kill[{bi}] ({body}): parse error: {e}")
            boss_kill_predicates.append(b"\x0d\x00")  # safe default: FALSE

    # Cave entrance-shuffle source gates from entrance_registry.yaml. Omitted or
    # TRUE() rows encode as len 0, meaning the source region alone gates the slot.
    cave_source_predicates: list[bytes] = []
    for idx, (interior_id, src) in enumerate(
            load_cave_source_predicates(RANDO_ASSETS / "entrance_registry.yaml")):
        if src.strip() == "TRUE()":
            cave_source_predicates.append(b"")
            continue
        try:
            ast = parse_predicate(src)
            ast = resolve_calls(ast, ops, all_macros)
            ast = expand_macros(ast, all_macros, parsed_macro_bodies, ops)
            errs = well_formedness(ast, ops, items, logic_regions, locations,
                                   f"cave_source[{idx}]")
            if errs:
                for e in errs:
                    all_errors.append(f"cave_source[{idx}] {interior_id}: {e}")
            cave_source_predicates.append(encode_predicate(ast, ops, items, logic_regions, locations))
        except ParseError as e:
            all_errors.append(f"cave_source[{idx}] {interior_id}: parse error: {e}")
            cave_source_predicates.append(b"\x0d\x00")  # safe default: FALSE

    if all_errors:
        for err in all_errors:
            print(f"WARN: {err}", file=sys.stderr)
        if args.strict:
            sys.exit(1)

    # --- Door shuffle (add-rando-door-shuffle) -----------------------------
    # Compile the door-table Vm-leaf predicates + portal gates, and wrap each
    # door-controlled location's can_reach as
    #   (NOT DOORS_ACTIVE(d) AND vanilla) OR (DOORS_ACTIVE(d) AND DOORS_LOC_REACHABLE(loc))
    # — evaluates to exactly the vanilla bytes' result while inactive.
    # This block runs AFTER the strict gate above, so its errors get their own
    # flush below (an unknown macro otherwise compiled a door predicate to
    # constant FALSE silently, even under --strict).
    n_errors_pre_door = len(all_errors)
    door_vm_preds: list[bytes] = []
    door_portal_rows: list[tuple] = []
    door_pot_rows: list[dict] = []
    door_pot_bridge_digest = 0
    door_enemy_drop_rows: list[dict] = []
    door_enemy_drop_bridge_digest = 0
    door_enemy_check_rows: list[dict] = []
    door_enemy_check_bridge_digest = 0
    door_manifest_path = Path("assets/rando/door_predicates.gen.json")
    door_portals_path = Path("assets/rando/door_portals.yaml")
    if door_manifest_path.exists():
        import json as _json
        door_man = _json.loads(door_manifest_path.read_text())

        def compile_door_src(src: str, label: str) -> bytes:
            try:
                d_ast = parse_predicate(src)
                d_ast = resolve_calls(d_ast, ops, all_macros)
                d_ast = expand_macros(d_ast, all_macros, parsed_macro_bodies, ops)
                d_errs = well_formedness(d_ast, ops, items, logic_regions, locations, "can_reach")
                if d_errs:
                    for e in d_errs:
                        all_errors.append(f"door {label}: {e}")
                return encode_predicate(d_ast, ops, items, logic_regions, locations)
            except ParseError as e:
                all_errors.append(f"door {label}: parse error: {e}")
                return b"\x0d\x00"  # FALSE — conservative (unparsed => impassable)

        for di, src in enumerate(door_man.get("predicates", [])):
            door_vm_preds.append(compile_door_src(src, f"vmpred[{di}] {src[:60]!r}"))

        if door_portals_path.exists():
            door_portals_doc = yaml.safe_load(door_portals_path.read_text())
            gates_by_name = {p["name"]: p for p in door_portals_doc.get("portals", [])}
            _sorted_rids = _region_ids_for_codegen(logic_regions) if logic_regions else []
            _rid_index = {r: i for i, r in enumerate(_sorted_rids)}
            for p in door_man.get("portals", []):
                g = gates_by_name.get(p["name"])
                if g is None:
                    all_errors.append(f"door portal {p['name']!r}: no door_portals.yaml row")
                    continue
                fr = g.get("fork_region")
                if fr is None:
                    fr_idx = 0xFFFF
                else:
                    fr_idx = _rid_index.get(fr)
                    if fr_idx is None:
                        all_errors.append(f"door portal {p['name']!r}: unknown fork region {fr!r}")
                        continue
                enc = compile_door_src(g["predicate"], f"portal {p['name']}") if g.get("predicate") else b""
                door_portal_rows.append((p["dungeon"], p["is_drop"], p["door_region"],
                                         fr_idx, enc, p["name"]))
        else:
            all_errors.append("door_portals.yaml missing (door tables present)")

        if door_pot_sources:
            door_pot_rows, door_pot_bridge_digest = build_door_pot_bridge(
                door_pot_sources, compile_door_src)
        if door_enemy_drop_sources:
            door_enemy_drop_rows, door_enemy_drop_bridge_digest = build_door_enemy_drop_bridge(
                door_enemy_drop_sources, compile_door_src)
        if door_enemy_check_sources:
            door_enemy_check_rows, door_enemy_check_bridge_digest = build_door_enemy_check_bridge(
                door_enemy_check_sources, compile_door_src)

        _OP_AND, _OP_OR, _OP_NOT, _OP_DA, _OP_DLR = 12, 13, 14, 20, 21
        _loc_by_id = {l.id: (name, l) for name, l in locations.items()}
        _door_wrapped_ids = set()

        def wrap_door_location(fid: int, dgn: int, label: str) -> None:
            pair = _loc_by_id.get(fid)
            if pair is None:
                all_errors.append(f"door location id {fid}: not in location registry")
                return
            loc_key, _loc = pair
            enc = location_predicates.get(loc_key)
            if enc is None:
                all_errors.append(f"door location {loc_key!r}: no compiled predicates")
                return
            if fid in _door_wrapped_ids:
                return
            _door_wrapped_ids.add(fid)
            v = enc["can_reach"]
            enc["can_reach"] = (bytes([_OP_OR, 2, _OP_AND, 2, _OP_NOT, _OP_DA, dgn]) + v +
                                bytes([_OP_AND, 2, _OP_DA, dgn,
                                       _OP_DLR, fid & 0xFF, (fid >> 8) & 0xFF]))

        for entry in door_man.get("locations", []):
            wrap_door_location(entry["fork_id"], entry["dungeon"], "door")
        for row in door_pot_rows:
            wrap_door_location(row["loc_id"], row["dungeon"], "door pot")
        for row in door_enemy_drop_rows:
            wrap_door_location(row["loc_id"], row["dungeon"], "door enemy drop")
        for row in door_enemy_check_rows:
            wrap_door_location(row["loc_id"], row["dungeon"], "door enemy check")

    # Flush door-compile errors collected after the main strict gate (earlier
    # errors were already printed there; only the door block's are new).
    if len(all_errors) > n_errors_pre_door:
        for err in all_errors[n_errors_pre_door:]:
            print(f"ERROR: {err}", file=sys.stderr)
        # A door predicate that fails to compile becomes a constant-FALSE edge,
        # which silently walls off door-shuffle reachability (the silver-arrows
        # bug class) and can certify placements against a wrong graph. That must
        # never ship, so fail the build UNCONDITIONALLY — not only under CI
        # --strict — because the local Make / MSBuild codegen invokes this script
        # WITHOUT --strict. The committed door_predicates.gen.json
        # compiles clean, so this never fires on an unmodified tree.
        sys.exit(1)

    # Hints v2 metadata is validated only after every optional generated
    # location family has joined the registry, so compact-name collisions in a
    # local pot/enemy/terrain/bonk substrate fail the same codegen invocation
    # that would otherwise ship them.
    hint_registry_contract = validate_hint_registry_contract(
        hint_registry_contract_path,
        locations,
        logic_regions,
        _all_rids,
        world_state_overrides,
        base_location_count=len(contract_locations),
        base_region_count=len(contract_region_ids),
        update=args.update_hint_metadata_lock and not args.keep_hint_registry_contract,
    )
    hint_metadata = load_hint_metadata(
        hint_metadata_path,
        items,
        locations,
        contract_locations=contract_locations,
        regions=logic_regions,
        region_ids=_all_rids,
        contract_regions=contract_regions,
        contract_region_ids=contract_region_ids,
        contract_world_state_overrides=contract_world_state_overrides,
        registry_contract=hint_registry_contract,
    )
    validate_hint_metadata_lock(
        hint_metadata_lock_path,
        hint_metadata,
        hint_runtime_algorithm_version(RANDO_SRC / "rando_hints.h"),
        hint_runtime_text_schema_version(RANDO_SRC / "rando_hints.h"),
        update=args.update_hint_metadata_lock,
    )
    if args.update_hint_metadata_lock and not args.keep_hint_registry_contract:
        atomic_write_text(
            hint_registry_contract_path,
            json.dumps(
                hint_registry_contract, indent=2, sort_keys=True
            ) + "\n",
        )

    # Emit artifacts.
    out_headers = Path(args.out_headers)
    out_data = Path(args.out_data)
    out_headers.mkdir(parents=True, exist_ok=True)
    out_data.mkdir(parents=True, exist_ok=True)
    emit_location_ids(locations, out_headers / "location_ids.h")
    emit_item_ids(items, out_headers / "item_ids.h")
    emit_hint_metadata(hint_metadata, out_headers / "hint_metadata.h")
    emit_logic_data(locations, logic_regions, logic_edges, location_predicates, edge_predicates,
                    out_data / "logic_data.c", items=items, logic_loc_preds=logic_loc_preds,
                    ow_meta=ow_meta, ow_non_warp_edges=ow_non_warp_edge_count,
                    ow_regions_first=_ow_first,
                    compiled_overrides=compiled_overrides,
                    compiled_world_state_edges=compiled_world_state_edges,
                    trick_status_rows=trick_status_rows,
                    glitch_status_rows=glitch_status_rows,
                    boss_kill_predicates=boss_kill_predicates,
                    dungeon_vanilla_boss=DUNGEON_VANILLA_BOSS,
                    cave_source_predicates=cave_source_predicates,
                    door_vm_preds=door_vm_preds,
                    door_portal_rows=door_portal_rows,
                    door_pot_rows=door_pot_rows,
                    door_pot_bridge_digest=door_pot_bridge_digest,
                    door_enemy_drop_rows=door_enemy_drop_rows,
                    door_enemy_drop_bridge_digest=door_enemy_drop_bridge_digest,
                    door_enemy_check_rows=door_enemy_check_rows,
                    door_enemy_check_bridge_digest=door_enemy_check_bridge_digest,
                    souls_baked=souls_baked,
                    soul_pin_rooms=soul_pin_rooms)
    chest_lookup_count = emit_chest_lookup(locations, out_headers / "chest_lookup.h")
    pot_lookup_count = emit_pot_lookup(pot_lookup_rows, out_headers / "pot_lookup.h")
    terrain_lookup_count = emit_terrain_lookup(
        terrain_lookup_rows, out_headers / "terrain_lookup.h")

    bonk_lookup_count = emit_bonk_lookup(
        bonk_lookup_rows, out_headers / "bonk_lookup.h")
    print(f"  bonk_lookup.h: {bonk_lookup_count} rows")
    enemy_drop_lookup_count = emit_enemy_drop_lookup(
        enemy_drop_lookup_rows,
        items,
        out_headers / "enemy_drop_lookup.h",
    )
    enemy_check_lookup_count = emit_enemy_check_lookup(
        enemy_check_lookup_rows,
        out_headers / "enemy_check_lookup.h",
    )
    pot_nonpot_drop_count = emit_pot_nonpot_drop_counts(
        load_pot_nonpot_drop_counts(RANDO_ASSETS / "pot_key_depth.gen.yaml"),
        items,
        out_headers / "pot_nonpot_drop_counts.h",
    )
    icon_atlas_count = emit_icon_atlas(
        Path("assets/rando/icon_atlas.yaml"),
        out_headers / "icon_atlas.h",
    )
    direct_grant_icon_count = emit_direct_grant_icons(
        Path("assets/rando/direct_grant_icons.yaml"),
        items,
        out_headers / "direct_grant_icons.h",
    )

    print(f"generated location_ids.h ({len(locations)} locations)")
    print(f"generated item_ids.h ({len(items)} items)")
    print(
        "generated hint_metadata.h "
        f"({len(hint_metadata.items)} items, "
        f"{len(hint_metadata.locations)} locations)"
    )
    print(f"generated logic_data.c ({len(logic_regions)} regions, {len(logic_edges)} edges, {len(locations)} locations)")
    print(f"generated chest_lookup.h ({chest_lookup_count} chest entries)")
    print(f"generated pot_lookup.h ({pot_lookup_count} pot entries)")
    print(f"generated enemy_drop_lookup.h ({enemy_drop_lookup_count} forced enemy-drop entries)")
    print(f"generated enemy_check_lookup.h ({enemy_check_lookup_count} ordinary enemy-check entries)")
    print(f"generated pot_nonpot_drop_counts.h ({pot_nonpot_drop_count} free-grant entries)")
    print(f"generated icon_atlas.h ({icon_atlas_count} icon entries)")
    print(f"generated direct_grant_icons.h ({direct_grant_icon_count} mapped icons)")
    print(f"warnings: {len(all_errors)}, macro errors: {len(macro_errors)}")


if __name__ == "__main__":
    main()
