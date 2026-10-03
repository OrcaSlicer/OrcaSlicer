#!/usr/bin/env python3
"""Exercise painted 3MF and Bambu defaults through Bambuddy's CLI argument shape."""
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import zipfile


def write_json(path, value):
    path.write_text(json.dumps(value))
    return str(path)


def cube_3mf(path, painted):
    vertices = [(x, y, z) for z in (0, 5) for y in (0, 10) for x in (0, 10)]
    faces = ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4),
             (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5))
    triangles = []
    for i, (a, b, c, d) in enumerate(faces):
        paint = f' paint_color="{4 if i % 2 else 8}"' if painted else ""
        for p, q, r in ((a, b, c), (a, c, d)):
            triangles.append(f'<triangle v1="{p}" v2="{q}" v3="{r}"{paint}/>')
    mesh = '<vertices>' + ''.join(f'<vertex x="{x}" y="{y}" z="{z}"/>'
                                 for x, y, z in vertices) + '</vertices>'
    mesh += '<triangles>' + ''.join(triangles) + '</triangles>'
    model = ('<model xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" unit="millimeter">'
             '<metadata name="BambuStudio:MmPaintingVersion">0</metadata>'
             '<resources><object id="1" type="model"><mesh>' + mesh + '</mesh></object></resources>'
             '<build><item objectid="1"/></build></model>')
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('3D/3dmodel.model', model)
        archive.writestr('_rels/.rels', '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
                         '<Relationship Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel" '
                         'Target="/3D/3dmodel.model" Id="rel0"/></Relationships>')
        archive.writestr('[Content_Types].xml', '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                         '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>'
                         '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/></Types>')


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    if not binary.is_file():
        return 77
    with tempfile.TemporaryDirectory(prefix='orca-bambuddy-') as temporary:
        root = pathlib.Path(temporary)
        machine = write_json(root / 'machine.json', {
            'type': 'machine', 'from': 'User', 'name': 'CLI single nozzle',
            'nozzle_diameter': ['0.4'], 'single_extruder_multi_material': '1',
            'printable_area': ['0x0', '200x0', '200x200', '0x200'],
            'printable_height': '100', 'layer_change_gcode': 'G92 E0'})
        process = write_json(root / 'process.json', {
            'type': 'process', 'from': 'User', 'name': 'CLI process',
            'enable_prime_tower': '0', 'layer_height': '0.2'})
        filaments = [write_json(root / f'filament{i}.json', {
            'type': 'filament', 'from': 'User', 'name': f'CLI PLA {i}',
            'filament_type': ['PLA'], 'filament_diameter': ['1.75']}) for i in range(4)]

        def run(name, source, count, expected=0):
            out = root / name
            out.mkdir()
            result = subprocess.run([
                str(binary), '--datadir', str(root / 'data'), '--slice', '1',
                '--allow-newer-file', '--load-settings', f'{machine};{process}',
                '--load-filaments', ';'.join(filaments[:count]),
                '--export-3mf', 'result.3mf', '--outputdir', str(out), str(source)
            ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
            if result.returncode != expected:
                raise AssertionError(f'{name}: exit {result.returncode}, expected {expected}\n{result.stdout[-12000:]}')
            if expected:
                assert 'tree_support_wall_count' in result.stdout, result.stdout
                return None
            artifact = out / 'result.3mf'
            with zipfile.ZipFile(artifact) as archive:
                gcodes = [n for n in archive.namelist() if n.endswith('.gcode')]
                assert gcodes, f'{name}: exported project contains no G-code'
                gcode = '\n'.join(archive.read(n).decode() for n in gcodes)
                assert re.search(r'^G1 .*E', gcode, re.M), f'{name}: no extrusion moves'
                if name.startswith('painted'):
                    assert re.search(r'^T1\s', gcode, re.M), f'{name}: second filament was lost'
            print(f'PASS {name}', flush=True)
            return artifact

        plain = root / 'plain.3mf'
        painted = root / 'painted.3mf'
        cube_3mf(plain, False)
        cube_3mf(painted, True)
        exported = run('plain', plain, 1)
        run('painted-two', painted, 2)
        run('painted-four', painted, 4)

        for wall_count, expected in (('-1', 0), ('-2', 238)):
            source = root / f'sentinels-{wall_count}.3mf'
            with zipfile.ZipFile(exported) as original, zipfile.ZipFile(source, 'w', zipfile.ZIP_DEFLATED) as modified:
                for item in original.infolist():
                    content = original.read(item.filename)
                    if item.filename == 'Metadata/project_settings.config':
                        settings = json.loads(content)
                        settings.update(wall_filament='0', sparse_infill_filament='0',
                                        solid_infill_filament='0', tree_support_wall_count=wall_count,
                                        prime_tower_brim_width='-1')
                        content = json.dumps(settings).encode()
                    modified.writestr(item, content)
            run(f'sentinels-{wall_count}', source, 1, expected)
    return 0


if __name__ == '__main__':
    sys.exit(main())
