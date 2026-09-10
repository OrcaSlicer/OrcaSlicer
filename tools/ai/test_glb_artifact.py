"""Synthetic local artifacts and a fully mocked provider: never calls a service."""
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from PIL import Image
from glb_artifact import Glb, GlbError, prepare_generated_glb, write_analysis_obj


def fixture(path, transform=False):
    positions = [(0, 0, 0), (.06, 0, 0), (0, .1, 0), (0, 0, -.04)]
    uvs = [(.25, .25), (.75, .25), (.25, .75), (.75, .75)]
    vertices = b''.join(struct.pack('<3f2f4B', *p, *uv, 255, 255, 255, 255) for p, uv in zip(positions, uvs))
    faces = struct.pack('<12H', 0, 2, 1, 0, 1, 3, 1, 2, 3, 2, 0, 3)
    texture = Image.new('RGB', (2, 2))
    texture.putdata([(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)])
    image = io.BytesIO(); texture.save(image, format='PNG')
    data = vertices + faces + image.getvalue()
    doc = {
        'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0]}],
        'nodes': [{'mesh': 0}], 'buffers': [{'byteLength': len(data)}],
        'bufferViews': [{'buffer': 0, 'byteLength': len(vertices), 'byteStride': 24},
                        {'buffer': 0, 'byteOffset': len(vertices), 'byteLength': len(faces)},
                        {'buffer': 0, 'byteOffset': len(vertices)+len(faces), 'byteLength': len(image.getvalue())}],
        'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': 4, 'type': 'VEC3', 'min': [0,0,-.04], 'max': [.06,.1,0]},
                      {'bufferView': 0, 'byteOffset': 12, 'componentType': 5126, 'count': 4, 'type': 'VEC2'},
                      {'bufferView': 0, 'byteOffset': 20, 'componentType': 5121, 'normalized': True, 'count': 4, 'type': 'VEC4'},
                      {'bufferView': 1, 'componentType': 5123, 'count': 12, 'type': 'SCALAR'}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0, 'TEXCOORD_0': 1, 'COLOR_0': 2}, 'indices': 3, 'material': 0}]}],
        'materials': [{'pbrMetallicRoughness': {'baseColorTexture': {'index': 0}, 'baseColorFactor': [.5,.5,.5,1]}}],
        'textures': [{'source': 0, 'sampler': 0}], 'samplers': [{'wrapS': 33071, 'wrapT': 33071}],
        'images': [{'bufferView': 2, 'mimeType': 'image/png'}],
    }
    if transform:
        doc['nodes'] = [{'children': [1,2], 'translation': [.1,.2,.3]},
                        {'mesh': 0, 'scale': [2,1,1]}, {'mesh': 0, 'scale': [-1,1,1]}]
        doc['materials'][0]['pbrMetallicRoughness']['baseColorTexture']['extensions'] = {
            'KHR_texture_transform': {'offset': [1,0], 'scale': [-1,1]}}
    write_glb(path, doc, data)
    return doc, data


def write_glb(path, doc, data):
    encoded = json.dumps(doc).encode('utf-8'); encoded += b' ' * (-len(encoded) % 4)
    data += b'\0' * (-len(data) % 4)
    Path(path).write_bytes(struct.pack('<4sII', b'glTF', 2, 28+len(encoded)+len(data)) +
                          struct.pack('<II', len(encoded), 0x4e4f534a) + encoded +
                          struct.pack('<II', len(data), 0x004e4942) + data)


class GlbArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source.glb'
        self.doc, self.data = fixture(self.source)

    def test_embedded_texture_factor_and_normalized_interleaved_vertex_colors(self):
        mesh = Glb(self.source).mesh()
        for color, expected in zip(mesh.colors, [(1,0,0), (0,1,0), (0,0,1), (1,1,1)]):
            for a, b in zip(color, expected): self.assertAlmostEqual(a, b * .735356983, places=6)
        self.assertEqual(len(mesh.faces), 4)

    def test_placement_preserves_embedded_data_and_original_bytes(self):
        before = self.source.read_bytes()
        output = self.root / 'model.glb'; analysis = self.root / 'analysis.obj'
        prepare_generated_glb(self.source, output, analysis)
        self.assertEqual(self.source.read_bytes(), before)
        self.assertEqual(Glb(self.source).binary, Glb(output).binary)
        self.assertEqual(Glb(output).doc['materials'], self.doc['materials'])
        vertices = [[float(v) for v in line.split()[1:4]] for line in analysis.read_text().splitlines() if line.startswith('v ')]
        spans = [max(v[i] for v in vertices)-min(v[i] for v in vertices) for i in range(3)]
        for actual, expected in zip(spans, [60,40,100]): self.assertAlmostEqual(actual, expected, places=4)
        self.assertAlmostEqual(min(v[2] for v in vertices), 0)

    def test_node_instances_negative_scale_and_uv_transform(self):
        fixture(self.source, True)
        mesh = Glb(self.source).mesh()
        self.assertEqual((len(mesh.vertices), len(mesh.faces)), (8,8))
        self.assertAlmostEqual(mesh.vertices[1][0], .22, places=6)
        self.assertEqual(mesh.faces[4], (4,5,6))
        self.assertAlmostEqual(mesh.colors[0][1], .735356983, places=6)
        self.assertAlmostEqual(mesh.colors[0][0], 0)

    def test_invalid_bounds_external_resources_cycles_and_extensions_fail(self):
        for change in ('bounds', 'external', 'cycle', 'extension', 'sparse', 'index'):
            with self.subTest(change=change):
                doc = copy.deepcopy(self.doc); data = self.data
                if change == 'bounds': doc['accessors'][0]['count'] = 100
                if change == 'external': doc['buffers'][0]['uri'] = 'https://example.invalid/model.bin'
                if change == 'cycle': doc['nodes'][0]['children'] = [0]
                if change == 'extension': doc['extensionsRequired'] = ['KHR_draco_mesh_compression']
                if change == 'sparse': doc['accessors'][0]['sparse'] = {}
                if change == 'index': data = data[:96] + struct.pack('<H', 99) + data[98:]
                write_glb(self.source, doc, data)
                with self.assertRaises(GlbError): Glb(self.source).mesh()

    def test_direct_generation_publishes_glb_and_never_submits_conversion(self):
        import orca_ai_sidecar as sidecar
        raw = self.source.read_bytes()
        with mock.patch.dict(os.environ, {'ORCASLICER_AI_OUTPUT_DIR': str(self.root / 'jobs')}):
            job = sidecar._new_job('text', ())
            gateway = mock.Mock()
            gateway.start_or_reuse_model_task.return_value = mock.Mock(task_id='generated-model', provider='tripo', reused=False)
            gateway.wait_for_task.return_value = {'model': 'mock-only'}
            gateway.download_artifact.side_effect = lambda result, path, maximum: path.write_bytes(raw)
            with mock.patch.object(sidecar, '_MODEL_PROVIDER_GATEWAY', gateway), \
                 mock.patch.object(sidecar, '_automatic_visual_review'):
                sidecar._generate_job(job, 'synthetic model', False, sidecar.PaidTaskAuthorization.confirmed(f'{job.id}:model:1'))
            self.assertEqual(job.state, 'ready', job.message)
            self.assertEqual(job.artifact_format, 'glb')
            gateway.start_or_reuse_conversion.assert_not_called()
            self.assertEqual((job.directory / 'provider-model.glb').read_bytes(), raw)
            self.assertTrue(sidecar._analysis_artifact(job.artifact_path).is_file())
            public = sidecar._public_job(job)
            self.assertEqual(public['artifact']['format'], 'glb')
            self.assertEqual(public['artifact']['color_encoding'], 'textures_or_vertex_colors')
            self.assertTrue(public['artifact']['ready'])
            self.assertTrue(sidecar._validate_artifact(job.artifact_path, 'glb'))

    def test_legacy_submitted_obj_conversion_is_reused(self):
        import orca_ai_sidecar as sidecar
        with mock.patch.dict(os.environ, {'ORCASLICER_AI_OUTPUT_DIR': str(self.root / 'jobs')}):
            job = sidecar._new_job('text', ())
            job.attempts = [{'conversion_task_id': 'already-paid-conversion'}]
            with mock.patch.object(sidecar, '_download_conversion', return_value=self.root / 'legacy.obj') as download:
                result = sidecar._download_generation_artifact(job, 'generation', 1, True)
            self.assertEqual(result.suffix, '.obj')
            download.assert_called_once_with(job, 'generation', 'obj', 1, True)

    def test_resume_reuses_the_complete_glb_after_download(self):
        import orca_ai_sidecar as sidecar
        with mock.patch.dict(os.environ, {'ORCASLICER_AI_OUTPUT_DIR': str(self.root / 'jobs')}):
            job = sidecar._new_job('text', ())
            directory = job.directory / 'attempt-01'; directory.mkdir()
            output = directory / 'model.glb'
            prepare_generated_glb(self.source, output, directory / 'analysis-model.obj')
            with mock.patch.object(sidecar, '_MODEL_PROVIDER_GATEWAY') as gateway:
                result = sidecar._download_generation_artifact(job, 'existing-generation', 1, True)
            self.assertEqual(result, output)
            self.assertEqual(gateway.mock_calls, [])


if __name__ == '__main__':
    unittest.main()
