#!/usr/bin/env python3
"""CORS, security headers, proxy IP and rate limiting from std.web."""

from harness import *  # noqa: F401,F403


def run_cases():
    run_skein("web.skein")
