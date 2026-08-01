#!/usr/bin/env python3
"""Regenerate src/EmbeddedOffsets.h from item JSON plus existing FRIK rows.

FRIK rows are retained from the checked-in header because the item-offset
source tree and FRIK's weapon resources are maintained separately.  Keep the
capture-group mapping below in lock-step with EmbeddedOffsets::OffsetData.
"""

from __future__ import annotations

import json
import re
from datetime import datetime
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
WORKSPACE_ROOT = REPO_ROOT.parent
JSON_DIR = WORKSPACE_ROOT / "Item offsets" / "item_offsets"
OUTPUT_HEADER = REPO_ROOT / "src" / "EmbeddedOffsets.h"


def parse_item_json(path: Path) -> dict:
    root = json.loads(path.read_text(encoding="utf-8"))
    name, entry = next(iter(root.items()))
    variant = entry.get("variant", {})
    is_left = variant.get("isLeftHanded", False)
    is_pa = variant.get("isPowerArmor", False)
    is_throwable = variant.get("isThrowable", False)

    if path.stem.endswith("_L") and not variant:
        is_left = True

    is_right_hand = path.stem.endswith("_R")
    is_frik = is_right_hand and "fingerCurls" not in entry
    position = entry.get("position", {})
    rotation = list(entry.get("rotation", []))
    rotation.extend([0.0] * (12 - len(rotation)))
    dimensions = entry.get("dimensions", {})
    curls = entry.get("fingerCurls", {})
    if not isinstance(curls, dict):
        curls = {}

    form_id = entry.get("formId", "")
    if form_id:
        form_id = (form_id.upper().removeprefix("0X").lstrip("0") or "0").zfill(8)

    return {
        "name": name,
        "posX": position.get("x", 0.0),
        "posY": position.get("y", 0.0),
        "posZ": position.get("z", 0.0),
        "rot": rotation[:12],
        "scale": entry.get("scale", 1.0),
        "dimL": dimensions.get("length", 0.0),
        "dimW": dimensions.get("width", 0.0),
        "dimH": dimensions.get("height", 0.0),
        "fingerDistance": entry.get("fingerDistance", 0.0),
        "fingerCurls": [
            curls.get("thumb", 1.0),
            curls.get("index", 1.0),
            curls.get("middle", 1.0),
            curls.get("ring", 1.0),
            curls.get("pinky", 1.0),
        ],
        "itemType": entry.get("itemType", ""),
        "formId": form_id,
        "isRightHandSpace": is_right_hand,
        "isFRIKOffset": is_frik,
        "isLeftHanded": is_left,
        "isPowerArmor": is_pa,
        "isThrowable": is_throwable,
    }


# Capture groups 14-18 are deliberately documented and regression-tested by
# tools/test_generate_offsets.py:
#   14=isRightHandSpace, 15=isFRIKOffset, 16=isLeftHanded,
#   17=isPowerArmor, 18=isThrowable.
FRIK_ROW_PATTERN = re.compile(
    r'\{\s*\n\s*"([^"]+)",\s*\n'
    r"\s*([-\d.f]+),\s*([-\d.f]+),\s*([-\d.f]+),\s*\n"
    r"\s*\{([^}]+)\},\s*\n"
    r"\s*([-\d.f]+),\s*\n"
    r"\s*([-\d.f]+),\s*([-\d.f]+),\s*([-\d.f]+),\s*\n"
    r"\s*([-\d.f]+),\s*\n"
    r"\s*\{([^}]+)\},\s*\n"
    r'\s*"([^"]*)",\s*\n'
    r'\s*"([^"]*)",\s*\n'
    r"\s*(true|false),\s*\n"
    r"\s*(true|false),\s*\n"
    r"\s*(true|false),\s*(true|false),\s*(true|false)\s*\n"
    r"\s*\}",
    re.MULTILINE,
)


def _floats(text: str) -> list[float]:
    return [float(value.strip().removesuffix("f")) for value in text.split(",")]


def extract_frik_offsets(header: Path) -> list[dict]:
    rows: list[dict] = []
    text = header.read_text(encoding="utf-8")
    for match in FRIK_ROW_PATTERN.finditer(text):
        # Do not shift these indices. Group 15, not group 14, is the FRIK flag.
        if match.group(15) != "true":
            continue
        rows.append(
            {
                "name": match.group(1),
                "posX": float(match.group(2).removesuffix("f")),
                "posY": float(match.group(3).removesuffix("f")),
                "posZ": float(match.group(4).removesuffix("f")),
                "rot": _floats(match.group(5)),
                "scale": float(match.group(6).removesuffix("f")),
                "dimL": float(match.group(7).removesuffix("f")),
                "dimW": float(match.group(8).removesuffix("f")),
                "dimH": float(match.group(9).removesuffix("f")),
                "fingerDistance": float(match.group(10).removesuffix("f")),
                "fingerCurls": _floats(match.group(11)),
                "itemType": match.group(12),
                "formId": match.group(13),
                "isRightHandSpace": match.group(14) == "true",
                "isFRIKOffset": True,
                "isLeftHanded": match.group(16) == "true",
                "isPowerArmor": match.group(17) == "true",
                "isThrowable": match.group(18) == "true",
            }
        )
    return rows


def _cpp_float(value: float) -> str:
    return f"{value:.8f}f"


def generate_entry(data: dict) -> str:
    rotation = ", ".join(_cpp_float(value) for value in data["rot"])
    curls = ", ".join(_cpp_float(value) for value in data["fingerCurls"])
    boolean = lambda value: "true" if value else "false"
    name = data["name"].replace('"', '\\"')
    return f"""    {{
        "{name}",
        {_cpp_float(data["posX"])}, {_cpp_float(data["posY"])}, {_cpp_float(data["posZ"])},
        {{ {rotation} }},
        {_cpp_float(data["scale"])},
        {_cpp_float(data["dimL"])}, {_cpp_float(data["dimW"])}, {_cpp_float(data["dimH"])},
        {_cpp_float(data["fingerDistance"])},
        {{ {curls} }},
        "{data["itemType"]}",
        "{data["formId"]}",
        {boolean(data["isRightHandSpace"])},
        {boolean(data["isFRIKOffset"])},
        {boolean(data["isLeftHanded"])}, {boolean(data["isPowerArmor"])}, {boolean(data["isThrowable"])}
    }}"""


def generate_header(item_rows: list[dict], frik_rows: list[dict]) -> str:
    item_names = {row["name"] for row in item_rows}
    retained_frik = [row for row in frik_rows if row["name"] not in item_names]
    all_rows = item_rows + retained_frik
    entries = ",\n".join(generate_entry(row) for row in all_rows)
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M")
    return f"""#pragma once
// Auto-generated from {len(item_rows)} JSON offset files + {len(retained_frik)} FRIK weapon offsets
// Generated: {timestamp}
//
// Offset variant flags: isLeftHanded, isPowerArmor, isThrowable
// Lookup key format: BaseName[_L][_PA][_T]

#include <array>
#include <string_view>

namespace EmbeddedOffsets {{

struct OffsetData {{
    std::string_view name;
    float posX, posY, posZ;
    float rot[12];
    float scale;
    float dimL, dimW, dimH;
    float fingerDistance;
    float fingerCurls[5];
    std::string_view itemType;
    std::string_view formId;
    bool isRightHandSpace;
    bool isFRIKOffset;
    bool isLeftHanded;
    bool isPowerArmor;
    bool isThrowable;
}};

inline constexpr std::array<OffsetData, {len(all_rows)}> kOffsets = {{{{
{entries}
}}}};

inline constexpr size_t kOffsetCount = {len(all_rows)};

}} // namespace EmbeddedOffsets
"""


def main() -> None:
    item_rows = [
        parse_item_json(path)
        for path in sorted(JSON_DIR.glob("*.json"))
    ]
    frik_rows = extract_frik_offsets(OUTPUT_HEADER)
    OUTPUT_HEADER.write_text(
        generate_header(item_rows, frik_rows),
        encoding="utf-8",
    )
    print(
        f"Wrote {OUTPUT_HEADER} "
        f"({len(item_rows)} item rows, {len(frik_rows)} FRIK rows read)"
    )


if __name__ == "__main__":
    main()
