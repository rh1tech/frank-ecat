#!/bin/bash
#
# frank-ecat — eCat3 (retro computers emulator) on RP2350 (FRANK M2)
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-ecat
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs the repository's git hooks (once per clone): commit-msg,
# applypatch-msg and pre-push refuse history that credits an AI
# (tools/check-attribution.sh).

set -e
cd "$(git rev-parse --show-toplevel)"
git config core.hooksPath .githooks
chmod +x .githooks/* tools/check-attribution.sh
echo "core.hooksPath = .githooks"
./tools/check-attribution.sh --self-test
