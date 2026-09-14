"""Artifact restart regressions with local files and fully mocked providers."""
import io
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

import orca_ai_sidecar as sidecar
from model_provider_gateway import ProviderTaskRef
from test_glb_artifact import fixture


class ArtifactRestartRecoveryTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        for patch in (
            mock.patch.object(sidecar, '_model_output_root', return_value=self.root),
            mock.patch.dict(sidecar._JOBS, {}, clear=True),
            mock.patch('urllib.request.OpenerDirector.open', side_effect=AssertionError('offline test')),
        ):
            patch.start()
            self.addCleanup(patch.stop)
        self.job = sidecar._new_job('text', ())

    def failed_direct_glb(self):
        self.job.state = self.job.phase = 'failed'
        self.job.output_format = 'glb'
        self.job.attempts = [{'attempt': 1, 'generation_task_id': 'existing-generation',
                              'status': 'rejected', 'error': 'artifact could not be downloaded'}]

    def test_restart_recovers_direct_glb_without_creating_any_paid_task(self):
        self.failed_direct_glb()
        sidecar._persist_job(self.job)
        with mock.patch.object(sidecar, '_submit') as submit:
            restored, = sidecar._restore_jobs(resume_jobs=False)
        self.assertEqual(restored.state, 'queued')
        self.assertEqual(restored.phase, 'resuming')
        submit.assert_not_called()
        source = self.root / 'source.glb'
        fixture(source)
        raw = source.read_bytes()
        gateway = mock.Mock()
        gateway.start_or_reuse_model_task.return_value = ProviderTaskRef('tripo', 'existing-generation', True)
        gateway.wait_for_task.return_value = {'model': 'mock-only'}
        gateway.download_artifact.side_effect = lambda result, path, maximum: path.write_bytes(raw)
        with mock.patch.object(sidecar, '_MODEL_PROVIDER_GATEWAY', gateway), \
             mock.patch.object(sidecar, '_automatic_visual_review'):
            sidecar._generate_job(restored, 'synthetic object', True)
        self.assertEqual(restored.state, 'ready', restored.message)
        self.assertEqual(gateway.start_or_reuse_model_task.call_args.kwargs['existing_task_id'], 'existing-generation')
        self.assertIsNone(gateway.start_or_reuse_model_task.call_args.kwargs['authorization'])
        gateway.start_or_reuse_conversion.assert_not_called()
        self.assertEqual(restored.artifact_format, 'glb')

    def test_restart_preserves_stop_missing_identity_and_nonretryable_failures(self):
        cases = (
            ('stopped', 'glb', {}),
            ('failed', 'obj', {}),
            ('failed', 'glb', {'generation_task_id': ''}),
            ('failed', 'glb', {'error': 'permanent model validation failure'}),
            ('failed', 'glb', {'conversion_submission_started': True}),
        )
        for state, output_format, changes in cases:
            with self.subTest(state=state, output_format=output_format, changes=changes):
                self.failed_direct_glb()
                self.job.state = state
                self.job.output_format = output_format
                self.job.attempts[0].update(changes)
                sidecar._persist_job(self.job)
                with mock.patch.object(sidecar, '_submit') as submit:
                    restored, = sidecar._restore_jobs(resume_jobs=False)
                self.assertEqual(restored.state, state)
                submit.assert_not_called()

    def test_hunyuan_obj_retry_keeps_partial_extractions_and_reuses_remote_task(self):
        self.job.provider, self.job.output_format = 'hunyuan', 'obj'
        self.job.attempts = [{'attempt': 1, 'provider': 'hunyuan', 'generation_task_id': 'paid-hunyuan'}]
        attempt = self.job.directory / 'attempt-01'
        package = attempt / 'package'
        package.mkdir(parents=True)
        partial = package / 'model.obj'
        partial.write_bytes(b'partial previous extraction')
        previous_recovery = attempt / 'recovery-01'
        previous_recovery.mkdir()
        previous_artifact = previous_recovery / 'preserved.txt'
        previous_artifact.write_bytes(b'previous recovery')
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, 'w') as bundle:
            bundle.writestr('model.obj', 'v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nf 1 2 3\n')
        gateway = mock.Mock()
        gateway.wait_for_task.return_value = {'artifact': 'mock-only'}
        gateway.download_artifact.side_effect = lambda result, path, maximum, **options: path.write_bytes(archive.getvalue())

        def extract_only(raw, directory, palette, palette_roles):
            # Exercise the real safe extractor; geometry processing is unrelated.
            return sidecar._extract_obj_package(raw, directory / 'package')

        with mock.patch.object(sidecar, '_HUNYUAN_PROVIDER_GATEWAY', gateway), \
             mock.patch.object(sidecar, '_prepare_obj_artifact', side_effect=extract_only):
            result = sidecar._download_generation_artifact(self.job, 'paid-hunyuan', 1, True)
        self.assertEqual(result, attempt / 'recovery-02' / 'package' / 'model.obj')
        self.assertTrue(result.read_text().endswith('f 1 2 3\n'))
        self.assertEqual(partial.read_bytes(), b'partial previous extraction')
        self.assertEqual(previous_artifact.read_bytes(), b'previous recovery')
        gateway.wait_for_task.assert_called_once_with('paid-hunyuan', stop_event=self.job.stop_event)
        gateway.start_or_reuse_model_task.assert_not_called()
        gateway.start_or_reuse_conversion.assert_not_called()


if __name__ == '__main__':
    unittest.main()
