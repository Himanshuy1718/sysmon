#!/usr/bin/env python3
"""Simulates a memory leak: grows by STEP_MB every second up to LIMIT_MB,
holds briefly, then exits (the OS frees everything).

Usage: python3 scripts/leak_memory.py [step_mb=40] [limit_mb=2500]
Keep limit_mb well below your free memory (check with: free -m).
"""
import sys
import time

step_mb = int(sys.argv[1]) if len(sys.argv) > 1 else 40
limit_mb = int(sys.argv[2]) if len(sys.argv) > 2 else 2500

chunks = []
print(f"leaking {step_mb} MB/s up to {limit_mb} MB (Ctrl+C to stop)")
try:
    while len(chunks) * step_mb < limit_mb:
        # Filling with a non-zero byte forces the OS to really allocate the pages.
        chunks.append(bytearray(b"\x01") * (step_mb * 1024 * 1024))
        print(f"\r  holding {len(chunks) * step_mb} MB", end="", flush=True)
        time.sleep(1)
    print("\nlimit reached; holding for 20 s")
    time.sleep(20)
except KeyboardInterrupt:
    pass
print("\nreleasing memory.")
