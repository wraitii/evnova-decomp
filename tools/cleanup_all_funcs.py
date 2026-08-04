#!/usr/bin/env python3
"""Normalize ghidra function output to one-line-per-func format."""

import re
import sys
import subprocess

def normalize_func(line):
    """Parse and normalize a function entry."""
    # Split into offset, name, rest
    parts = line.split(maxsplit=2)
    if len(parts) < 2:
        return line
    
    offset, name = parts[0], parts[1]
    
    # Extract comment if present
    comment = ""
    if len(parts) == 3:
        comment_match = re.search(r'/\* ?(.*?) ?\*/', parts[2], re.DOTALL)
        if comment_match:
            comment_text = re.sub(r'\s+', ' ', comment_match.group(1)).strip()
            comment = f" /* {comment_text} */"
    
    return f"{offset} {name}(...){comment}"

def main():
    # Default to curling ghidra, or read from file if arg provided
    if len(sys.argv) > 1:
        with open(sys.argv[1], 'r') as f:
            content = f.read()
    else:
        # Use curl to fetch from ghidra API
        result = subprocess.run(
            ['curl', '-s', 'http://127.0.0.1:8166/functions?name_re=.*'],
            capture_output=True,
            text=True
        )
        content = result.stdout
    
    # Normalize newlines: join continuation lines (those not starting with address)
    lines = []
    for line in content.splitlines():
        if not line.strip():
            continue
        if re.match(r'^[0-9a-fA-F]{8} ', line):
            lines.append(line)
        elif lines:
            # Continuation of previous line
            lines[-1] += ' ' + line
    
    for line in lines:
        print(normalize_func(line))

if __name__ == "__main__":
    main()
