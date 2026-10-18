#!/usr/bin/env python3
"""Compile the actual hybrid policy header and test its integer boundary locally."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def run(repo, report):
    header = Path(repo) / 'cmd/zfs/chunkdedup_hybrid.h'
    cases = [
        (1000, 800, 0, 'CHUNK'), (1000, 801, 0, 'BLOCK'),
        (1000, 799, 0, 'CHUNK'), (1000, 0, 1, 'BLOCK'),
        (0, 0, 1, 'BLOCK'), (1001, 800, 0, 'CHUNK'),
        (1001, 801, 0, 'BLOCK'), (5, 4, 0, 'CHUNK'),
        (4, 4, 0, 'BLOCK'), (4, 3, 0, 'CHUNK'),
        ((1 << 64) - 1, (((1 << 64) - 1) * 4) // 5, 0, 'CHUNK'),
        ((1 << 64) - 1, (((1 << 64) - 1) * 4) // 5 + 1, 0, 'BLOCK'),
        ((1 << 64) - 1, (1 << 64) - 1, 0, 'BLOCK'),
    ]
    body = '#include <assert.h>\n#include <stdint.h>\n#include "chunkdedup_hybrid.h"\nint main(void) {\n'
    for native, chunk, uncertain, method in cases:
        body += f'assert(cd_hybrid_choose(UINT64_C({native}), UINT64_C({chunk}), {uncertain}) == CD_METHOD_{method});\n'
    body += 'return 0; }\n'
    with tempfile.TemporaryDirectory(prefix='hybrid-policy-', dir=Path(report).parent) as directory:
        source, binary = Path(directory) / 'policy.c', Path(directory) / 'policy'
        source.write_text(body)
        command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(header.parent), str(source), '-o', str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    result = {'schema': 1, 'status': 'PASS', 'scope': 'actual current hybrid selection policy only; no native operations',
              'header_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
              'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), 'cases': len(cases),
              'threshold_percent': 20, 'overflow_safe': True, 'uncertain_favors_native': True}
    Path(report).write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    options = parser.parse_args()
    print(json.dumps(run(options.repo, options.report), sort_keys=True))
