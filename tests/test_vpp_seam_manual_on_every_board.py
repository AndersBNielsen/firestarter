"""
Project Name: Firestarter
Copyright (c) 2024 Henrik Olsson
Permission is hereby granted under MIT license.

Compile and execute the production VPP seam under supported board macro sets; assert manual-only behavior and fail-closed compilation.
"""


import configparser
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_REPO_ROOT = _HERE.parent
_INCLUDE = _REPO_ROOT / "include"
_SEAM_HEADER = _INCLUDE / "rurp_vpp.h"
_SEAM_SRC = _REPO_ROOT / "src" / "rurp_vpp.cpp"
_PLATFORMIO_INI = _REPO_ROOT / "platformio.ini"
_PY32_CMAKE = _REPO_ROOT / "platform" / "py32f071" / "CMakeLists.txt"
_PY32_BOARD_HEADER = _INCLUDE / "boards" / "py32f071_rurp_shield.h"

_EXPECTED_MODE_MANUAL = 0
_EXPECTED_RESULT_MANUAL_REQUIRED = 1

def _expand(config, section, value):
    """Expand PlatformIO `${section.option}` references; `this` is the
    section being read."""
    def substitute(match):
        source = section if match.group(1) == "this" else match.group(1)
        return _expand(config, source, config.get(source, match.group(2)))

    return re.sub(r"\$\{(\w+)\.(\w+)\}", substitute, value)


def _platformio_macro_sets():
    """One macro set per AVR env in platformio.ini: its -D flags as
    PlatformIO passes them, plus `__AVR__`, which the AVR toolchain itself
    defines."""
    config = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=(";",))
    config.read(_PLATFORMIO_INI)
    sets = []
    for section in config.sections():
        if not section.startswith("env:") or config.get(section, "platform", fallback="") != "atmelavr":
            continue
        flags = _expand(config, section, config.get(section, "build_flags", fallback=""))
        defines = [d.replace('\\"', '"') for d in re.findall(r"-D\s*(\S+)", flags)]
        sets.append(pytest.param(("__AVR__", *defines), id=section[len("env:"):]))
    return sets


def _py32_macro_set():
    """The definitions the py32 CMake build passes to the firmware target."""
    match = re.search(
        r"target_compile_definitions\(\s*\$\{TARGET_NAME\}\s*PRIVATE(.*?)\)",
        _PY32_CMAKE.read_text(),
        re.S,
    )
    assert match, f"no target_compile_definitions block in {_PY32_CMAKE}"
    return pytest.param(tuple(match.group(1).split()), id="py32f071")


_BOARD_MACRO_SETS = [*_platformio_macro_sets(), _py32_macro_set()]


def test_every_real_build_has_a_macro_set():
    ids = [param.id for param in _BOARD_MACRO_SETS]
    assert len(ids) >= 2 and "py32f071" in ids, ids


def _resolve_compiler():
    """Resolve the host C++ compiler, fail-closed.

    Deliberately never bypassed via any decorator or runtime call that would
    mark this outcome as skipped, anywhere in this module: doing so would
    recreate the exact BASE-02/BASE-03 absence-proxy failure class this
    milestone's Phase 123 removed. If $CXX (or 'g++') cannot be resolved via
    shutil.which, this raises via a plain assert, which the test runner
    reports as a FAILURE, never a skipped outcome. This module's harness
    runs in NO CI leg on this branch (RESEARCH C-7) -- the local run is the
    only evidence, which makes the fail-closed contract here even more
    load-bearing than in the CI-covered precedent.
    """
    compiler = shutil.which(os.environ.get("CXX", "g++"))
    assert compiler is not None, (
        "host C++ compiler not found on PATH (checked $CXX, falling back to "
        "'g++'). This must FAIL the suite, never be silently skipped -- no "
        "embedded toolchain is invoked here."
    )
    return compiler


def _write_shim_tu(tmp_path):
    """Write the measured RESEARCH shim into tmp_path, fresh every call --
    never a committed fixture translation unit (D-03 declined one). Includes
    rurp_vpp.h and <cstdio>, and its main prints the control mode and the
    result of rurp_set_vpp_target_mv(12000, 200, 50) in the exact form
    'mode=%d result=%d', each cast to int, then calls
    rurp_disable_vpp_control() and returns 0 unconditionally."""
    tu_path = tmp_path / "vpp_seam_main.cpp"
    tu_path.write_text(
        '#include "rurp_vpp.h"\n'
        "#include <cstdio>\n"
        "int main(void) {\n"
        '    printf("mode=%d result=%d\\n", (int)rurp_vpp_control_mode(),\n'
        "           (int)rurp_set_vpp_target_mv(12000, 200, 50));\n"
        "    rurp_disable_vpp_control();\n"
        "    return 0;\n"
        "}\n"
    )
    return tu_path


def _compile(compiler, defines, sources, output_path):
    """Compile (and, when a real main is present, link) the given source
    files with the given macro definitions into a real binary at
    output_path. List argv only, never a shell; always passes
    -std=gnu++17 -Wall -Wextra and the include/ path. A definition whose
    value contains double quotes needs no extra escaping here -- there is
    no shell in the way, so the literal Python string is exactly what the
    compiler receives as argv."""
    argv = [compiler, "-std=gnu++17", "-Wall", "-Wextra", "-I", str(_INCLUDE)]
    argv += [f"-D{define}" for define in defines]
    argv += [str(source) for source in sources]
    argv += ["-o", str(output_path)]
    return subprocess.run(argv, capture_output=True, text=True)


def _run_binary(binary_path):
    """Execute the compiled binary as a second subprocess, list argv,
    capturing output. The seam's result value crosses this process boundary
    on stdout, never this run's exit code (Trap 2): 1 is also the exit code
    of a compile failure, a link failure and a crash, so returning it from
    main would make the correct answer indistinguishable from every
    failure mode."""
    return subprocess.run([str(binary_path)], capture_output=True, text=True)


def _expected_header_error_text():
    """Read the #error message out of include/rurp_vpp.h at test time,
    rather than hardcoding it a second time here -- a literal that could
    silently desync from the real header on a future edit. Scoped to this
    one file: the header and src/rurp_vpp.cpp each carry a DIFFERENT #error
    message, so this "exactly one directive" assertion must never be
    reused across both files."""
    text = _SEAM_HEADER.read_text()
    m = re.search(r'#\s*error\s+"([^"]*)"', text)
    assert m, (
        f"expected exactly one #error directive with a quoted message in "
        f"{_SEAM_HEADER}.\nGot:\n{text}"
    )
    return m.group(1)


def _expected_source_error_text():
    """Read the #error message out of src/rurp_vpp.cpp at test time, rather
    than hardcoding it a second time here. Scoped to this one file -- see
    _expected_header_error_text's docstring for why the two are separate
    helpers rather than one shared "exactly one" assertion."""
    text = _SEAM_SRC.read_text()
    m = re.search(r'#\s*error\s+"([^"]*)"', text)
    assert m, (
        f"expected exactly one #error directive with a quoted message in "
        f"{_SEAM_SRC}.\nGot:\n{text}"
    )
    return m.group(1)


@pytest.mark.parametrize("defines", _BOARD_MACRO_SETS)
def test_manual_control_on_every_board_macro_set(tmp_path, defines):
    """Coverage 1 -- one parametrized test asserting the seam refuses
    (control mode MANUAL, result MANUAL_ADJUSTMENT_REQUIRED) across all
    four board macro-sets in one run (ROADMAP Criterion 2), compiled AND
    run -- never a single-board spot check. Parses the result from stdout,
    never the run subprocess's exit code (Trap 2)."""
    compiler = _resolve_compiler()
    tu_path = _write_shim_tu(tmp_path)
    binary_path = tmp_path / "seam_under_test"

    compile_result = _compile(compiler, defines, (tu_path, _SEAM_SRC), binary_path)
    assert compile_result.returncode == 0, (
        f"expected a clean compile for board macro-set {defines!r}.\n"
        f"stdout:\n{compile_result.stdout}\nstderr:\n{compile_result.stderr}"
    )
    assert compile_result.stderr == "", (
        f"expected zero bytes of -Wall -Wextra warning output for board "
        f"macro-set {defines!r}.\n"
        f"stdout:\n{compile_result.stdout}\nstderr:\n{compile_result.stderr}"
    )

    run_result = _run_binary(binary_path)
    assert run_result.returncode == 0, (
        f"expected the compiled binary to run cleanly for board macro-set "
        f"{defines!r}.\n"
        f"stdout:\n{run_result.stdout}\nstderr:\n{run_result.stderr}"
    )

    match = re.match(r"mode=(\d+) result=(\d+)\s*$", run_result.stdout)
    assert match, (
        f"expected stdout of the form 'mode=<int> result=<int>' for board "
        f"macro-set {defines!r}.\n"
        f"stdout:\n{run_result.stdout!r}\nstderr:\n{run_result.stderr!r}"
    )
    mode, result = int(match.group(1)), int(match.group(2))
    assert mode == _EXPECTED_MODE_MANUAL, (
        f"expected rurp_vpp_control_mode() == RURP_VPP_CONTROL_MANUAL "
        f"({_EXPECTED_MODE_MANUAL}) for board macro-set {defines!r}, got "
        f"mode={mode}.\nstdout:\n{run_result.stdout}"
    )
    assert result == _EXPECTED_RESULT_MANUAL_REQUIRED, (
        f"expected rurp_set_vpp_target_mv() == "
        f"RURP_VPP_MANUAL_ADJUSTMENT_REQUIRED "
        f"({_EXPECTED_RESULT_MANUAL_REQUIRED}) for board macro-set "
        f"{defines!r}, got result={result}.\nstdout:\n{run_result.stdout}"
    )


def test_forced_capability_macro_fails_closed_in_the_source(tmp_path):
    """Coverage 2 -- D-03 via RESEARCH C-4: the header's
    `#if !defined(...)` guard cannot reject an explicitly forced
    RURP_HAS_VPP_DAC=1 (measured: exits 0 with only the header present), so
    this leg compiles src/rurp_vpp.cpp itself -- whose own, separately
    authored #error is the one that must fire. This is what makes the
    harness non-vacuous: a header-only leg would pass vacuously and the
    "the macro is genuinely consulted" argument would evaporate."""
    compiler = _resolve_compiler()
    expected_text = _expected_source_error_text()
    binary_path = tmp_path / "forced_capability"

    result = _compile(compiler, ("__AVR__", "RURP_HAS_VPP_DAC=1"), (_SEAM_SRC,), binary_path)

    assert result.returncode != 0, (
        "expected a non-zero compile exit with RURP_HAS_VPP_DAC forced to 1 "
        "while compiling src/rurp_vpp.cpp (the header alone cannot reject "
        f"this value).\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    assert expected_text in result.stderr, (
        f"expected src/rurp_vpp.cpp's own #error text in stderr.\n"
        f"Expected substring: {expected_text!r}\nGot stderr:\n{result.stderr}"
    )


def test_unset_and_non_avr_fails_closed_in_the_header(tmp_path):
    """Coverage 3 -- D-08: with neither __AVR__ nor an explicit
    RURP_HAS_VPP_DAC defined, include/rurp_vpp.h's #else arm must fire.
    This is the arm every native translation unit would reach were the
    seam ever included from include/rurp_shield.h (measured: pio test
    -e native goes to 17 suites / 0 succeeded) -- this leg is the standing
    explanation for why that include does not exist."""
    compiler = _resolve_compiler()
    tu_path = _write_shim_tu(tmp_path)
    expected_text = _expected_header_error_text()
    binary_path = tmp_path / "unset_non_avr"

    result = _compile(compiler, (), (tu_path,), binary_path)

    assert result.returncode != 0, (
        "expected a non-zero compile exit with neither __AVR__ nor "
        "RURP_HAS_VPP_DAC defined.\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )
    assert expected_text in result.stderr, (
        f"expected include/rurp_vpp.h's own #error text in stderr.\n"
        f"Expected substring: {expected_text!r}\nGot stderr:\n{result.stderr}"
    )
