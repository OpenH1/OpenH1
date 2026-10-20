"""Loading diagnostics keep whole records and preserve syscall errors.

These tests build only small stand-alone probes, without game data or a GPU.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
pytestmark = pytest.mark.skipif(sys.platform != "linux", reason="Linux trace syscall probes")


def build(tmp_path, source):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        pytest.skip("a C compiler is required")
    path = tmp_path / "probe.c"
    path.write_text(source)
    executable = tmp_path / "probe"
    subprocess.run([compiler, "-std=gnu89", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-I", str(ROOT / "port/linux/src"), str(path), "-lm", "-o", str(executable)], check=True)
    return executable


def test_concurrent_trace_records_are_whole_and_preserve_errno(tmp_path):
    executable = build(tmp_path, r'''
#include "load_trace.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>

static void *emit(void *argument)
{
    int index, owner = (int)(intptr_t)argument;
    for (index = 0; index < 500; index++)
    {
        errno = EDOM;
        load_trace_log("LOAD owner=%d sequence=%d detail=complete", owner, index);
        if (errno != EDOM)
            return (void *)1;
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    int index;
    char long_line[4096];
    for (index = 0; index < 4; index++)
        if (pthread_create(&threads[index], NULL, emit, (void *)(intptr_t)index))
            return 2;
    for (index = 0; index < 4; index++)
    {
        void *result;
        if (pthread_join(threads[index], &result) || result)
            return 3;
    }
    memset(long_line, 'x', sizeof(long_line));
    long_line[sizeof(long_line) - 1] = 0;
    load_trace_log("%s", long_line);
    return 0;
}
''')
    result = subprocess.run([str(executable)], capture_output=True, check=True, timeout=15)
    lines = result.stderr.splitlines()
    assert len(lines) == 2001
    expected = {f"LOAD owner={owner} sequence={index} detail=complete".encode()
                for owner in range(4) for index in range(500)}
    assert set(lines[:-1]) == expected
    assert lines[-1] == b"x" * 2046
    assert result.stderr.endswith(b"\n")
    assert os.path.getsize(executable) > 0


def test_failed_clock_does_not_hide_read_error_or_create_duration(tmp_path):
    text = (ROOT / "port/linux/src/xbox_files.c").read_text()
    # Compile the production helpers, not a copy of their logic.
    helpers = text[text.index("static double load_io_now(void)"):text.index("static BOOL read_at(")]
    source = r'''
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef int BOOL;
struct platform_file { int descriptor; };
static int failing_clock(clockid_t clock, struct timespec *value)
{
    (void)clock; (void)value;
    errno = ENOSYS;
    return -1;
}
static ssize_t failing_read(int descriptor, void *buffer, size_t count)
{
    (void)descriptor; (void)buffer; (void)count;
    errno = EIO;
    return -1;
}
static ssize_t failing_pread(int descriptor, void *buffer, size_t count, off_t offset)
{
    (void)offset;
    return failing_read(descriptor, buffer, count);
}
#define clock_gettime failing_clock
#define read failing_read
#define pread failing_pread
''' + helpers + r'''
int main(void)
{
    struct platform_file file = { 0 };
    char byte;
    double duration = 0.0;
    if (load_io_now() != -1.0)
        return 1;
    if (read_some(&file, &byte, 1, 1, 0, &duration) != -1 || errno != EIO || duration != -1.0)
        return 2;
    if (read_some(&file, &byte, 1, 0, 0, NULL) != -1 || errno != EIO)
        return 3;
    return 0;
}
'''
    executable = build(tmp_path, source)
    subprocess.run([str(executable)], check=True, timeout=5)
