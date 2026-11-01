#!/usr/bin/env python3
"""Emit ALttPR location IDs by cartridge index, without reading any ROM."""
from pathlib import Path
import importlib.util
import re

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = ROOT / 'engine/assets/chest_data.py'
    spec = importlib.util.spec_from_file_location('chest_data', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # These identifiers are emitted from the MIT location registry by codegen.
    identifiers = {}
    for line in (ROOT / 'engine/src/rando/location_ids.h').read_text().splitlines():
        match = re.match(r'#define\s+(LOC_\S+)\s+(\d+)', line)
        if match:
            identifiers[match[1]] = int(match[2])
    rows = []
    mapped = 0
    for index in range(module.CHEST_TABLE_ENTRY_COUNT):
        address = module.CHEST_TABLE_BASE_ADDR + index * 3
        entry = module.CHEST_NAME_BY_ROM_ADDR.get(address)
        if entry:
            name = 'LOC_' + re.sub(r'[^A-Za-z0-9_]', '_', entry[0])
            # Codegen collapses runs of punctuation into one underscore.
            name = re.sub('_+', '_', name).rstrip('_')
            if name not in identifiers:
                raise ValueError(f'Unknown location identifier: {name}')
            rows.append(name)
            mapped += 1
        else:
            rows.append('0xffff')
    text = ['// Generated from the MIT ALttPR catalog; no ROM bytes.',
            '// See rando/upstream/NOTICE and engine/assets/chest_data.py.',
            '#pragma once', '#include "location_ids.h"',
            'static const uint16 kChestLocationByCartridgeIndex[168] = {']
    text += ['  ' + ', '.join(rows[i:i+4]) + ',' for i in range(0, len(rows), 4)]
    text += ['};', f'enum {{ kChestCatalogLocationCount = {mapped} }};', '']
    (ROOT / 'engine/src/rando/chest_catalog.h').write_text('\n'.join(text))
    print(f'Wrote {mapped} location identities; no cartridge data read')


if __name__ == '__main__':
    main()
