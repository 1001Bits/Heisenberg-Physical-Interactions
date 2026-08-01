#!/usr/bin/env python3
"""Focused regression for EmbeddedOffsets boolean capture-group identity."""

import importlib.util
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("generate_offsets.py")
SPEC = importlib.util.spec_from_file_location("generate_offsets", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def main() -> None:
    row = """    {
        "Test Weapon",
        1.0f, 2.0f, 3.0f,
        { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f },
        1.0f,
        4.0f, 5.0f, 6.0f,
        7.0f,
        { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f },
        "WEAP",
        "00123456",
        true,
        true,
        false, true, false
    }"""
    match = MODULE.FRIK_ROW_PATTERN.search(row)
    assert match is not None
    assert match.group(14) == "true"
    assert match.group(15) == "true"
    assert match.group(16) == "false"
    assert match.group(17) == "true"
    assert match.group(18) == "false"
    print("generate_offsets capture-group regression passed")


if __name__ == "__main__":
    main()
