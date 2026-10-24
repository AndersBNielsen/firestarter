"""Compile and execute the production rollover-safe timeout predicate."""

import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

DRIVER = r"""
#include <assert.h>
#include <stdint.h>
#include "timeout_utils.h"

struct Clock {
    uint32_t now;
    void advance(uint32_t delta) { now += delta; }
    bool expired(uint32_t start, uint32_t duration) const {
        return fs_timeout_expired(start, duration, now);
    }
};

int main() {
    const uint32_t starts[] = {0U, 1U, UINT32_MAX - 5U, UINT32_MAX - 1U, UINT32_MAX};
    const uint32_t durations[] = {1U, 150U, 1000U, 15000U};
    for (uint32_t start : starts) {
        for (uint32_t duration : durations) {
            Clock clock{start};
            assert(!clock.expired(start, duration));
            clock.advance(duration - 1U);
            assert(!clock.expired(start, duration));
            clock.advance(1U);
            assert(clock.expired(start, duration));
            clock.advance(1U);
            assert(clock.expired(start, duration));
        }
    }
    Clock clock{UINT32_MAX - 3U};
    uint32_t start = clock.now;
    clock.advance(2U);
    assert(!clock.expired(start, 1000U));
    start = clock.now; // A refreshed watchdog gets a full interval.
    clock.advance(999U);
    assert(!clock.expired(start, 1000U));
    clock.advance(1U);
    assert(clock.expired(start, 1000U));

    for (uint32_t duration : durations) {
        Clock poll{UINT32_MAX - 2U};
        uint32_t started = poll.now;
        uint32_t iterations = 0;
        while (!poll.expired(started, duration) && iterations <= duration) {
            poll.advance(1U);
            ++iterations;
        }
        assert(iterations == duration);
        assert(poll.expired(started, duration));
    }
    assert(!fs_timeout_expired(0U, 1000U, 0U));
    assert(!fs_timeout_expired(0U, 1000U, 999U));
    assert(fs_timeout_expired(0U, 1000U, 1000U));
}
"""


def test_timeout_rollover_behavior(tmp_path: Path) -> None:
    cxx = shutil.which(os.environ.get("CXX", "g++"))
    assert cxx, "a host C++ compiler is required"
    source = tmp_path / "timer_regression.cpp"
    binary = tmp_path / "timer_regression"
    source.write_text(DRIVER, encoding="utf-8")
    subprocess.run(
        [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "include"), str(source), "-o", str(binary)],
        check=True, capture_output=True, text=True,
    )
    subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
