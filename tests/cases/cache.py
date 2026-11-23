#!/usr/bin/env python3
"""Optional Redis / Valkey std.cache tests."""

import os

from harness import *  # noqa: F401,F403


def run_cases():
    if os.environ.get("SKEIN_TEST_REDIS_DSN"):
        run_skein("cache.skein")
