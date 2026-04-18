#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Parameter validation for CLI template generator.

Validates op, threads, and object_size parameters before YAML generation.
"""

import re
from typing import List


VALID_OPS = {'upload', 'download'}

# Supported object size patterns: 1MB, 1GB, 1TB, 1KB, etc.
OBJECT_SIZE_PATTERN = re.compile(r'^\d+(\.\d+)?\s*(MB|GB|KB|TB|B|Bytes?)?$', re.IGNORECASE)


def validate_object_size_format(object_size: str) -> bool:
    """Check if object_size has a valid format.

    Args:
        object_size: The object size string to validate

    Returns:
        True if format is valid, False otherwise
    """
    if not object_size or object_size == '0':
        return False
    # Remove whitespace and check against pattern
    cleaned = object_size.strip()
    if not cleaned:
        return False
    return bool(OBJECT_SIZE_PATTERN.match(cleaned))


def validate_params(op: str, threads: int, object_size: str) -> List[str]:
    """Validate template generation parameters.

    Args:
        op: Operation type
        threads: Thread count
        object_size: Object size specification

    Returns:
        List of error messages (empty list means all parameters are valid)
    """
    errors = []

    if op not in VALID_OPS:
        errors.append(
            f"Invalid op '{op}'. Must be one of: {', '.join(sorted(VALID_OPS))}"
        )

    if threads <= 0:
        errors.append(f"threads must be > 0, got {threads}")

    if not validate_object_size_format(object_size):
        errors.append(
            f"object_size must be non-empty and in valid format (e.g., 1MB, 4MB, 1GB), "
            f"got '{object_size}'"
        )

    return errors
