# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""This machine's paths: the environment variable, else local.ini (not in the repository;
local.ini.example lists the keys), else the default."""
import os

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))


def get(key, default=None):
    v = os.environ.get(key)
    if v:
        return v
    path = os.path.join(REPO, 'local.ini')
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            line = line.strip()
            if line and not line.startswith(';') and '=' in line:
                k, _, val = line.partition('=')
                if k.strip() == key:
                    return val.strip()
    return default
