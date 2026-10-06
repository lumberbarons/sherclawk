#!/usr/bin/env python3
"""Embed the verified C/Rez template as CR source without maintaining copies.

Only editable source and a fixed versioned descriptor enter projects. Native
build recipes remain trusted executor inputs, never model-supplied scripts.
"""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
output = root / 'build/project-template.h'
output.parent.mkdir(parents=True, exist_ok=True)
entries = []
for name in ('main.c', 'app.r'):
    source = (root / 'templates/ppc-toolbox' / name).read_text()
    source.encode('ascii')  # C string escapes below must preserve exact bytes.
    assert len(source) <= 4096
    entries.append((name, source.replace('\n', '\r')))
entries.append(('project.json', '{"protocol":2,"toolchain":"mpw-ppc-v2","template":"ppc-toolbox-v1","sources":["main.c"],"resources":["app.r"],"headers":[],"include_paths":[],"output":"template","settings":{"warnings":"off","libraries":["InterfaceLib","StdCLib"],"creator":"SHTP"}}\r'))
with output.open('w') as stream:
    stream.write('/* Generated from templates/ppc-toolbox; do not edit. */\n')
    stream.write('static const struct { const char *name, *bytes; } project_inputs[] = {\n')
    for name, source in entries:
        stream.write('    {' + json.dumps(name) + ',\n')
        for line in source.splitlines(keepends=True):
            stream.write('        ' + json.dumps(line) + '\n')
        stream.write('    },\n')
    stream.write('};\n')
