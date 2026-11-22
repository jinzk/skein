#!/usr/bin/env python3
"""Cookie, session and token helpers from std.auth."""

from harness import *  # noqa: F401,F403


def run_cases():
    run_skein("auth.skein")
    run_skein("password.skein")
