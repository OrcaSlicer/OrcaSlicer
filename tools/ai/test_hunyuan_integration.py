"""Offline provider selection, paid-task identity and recovery checks."""
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import orca_ai_sidecar as sidecar
import hunyuan_provider_gateway as gateway_module
from model_provider_gateway import ModelTaskRequest, PaidTaskAuthorization, ProviderGatewayError, ProviderTaskRef


class HunyuanIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        patch = mock.patch.object(sidecar, '_model_output_root', return_value=self.root)
        patch.start()
        self.addCleanup(patch.stop)
        patch = mock.patch.dict(sidecar._JOBS, {}, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        # No test may accidentally contact either real provider.
        patch = mock.patch('urllib.request.OpenerDirector.open', side_effect=AssertionError('offline test'))
        patch.start()
        self.addCleanup(patch.stop)
        self.job = sidecar._new_job('text', ())

    def rejected_design(self):
        self.job.provider = 'hunyuan'
        self.job.source = 'image'
        self.job.state = self.job.phase = 'failed'
        self.job.preview_path = self.job.directory / 'preview.png'
        self.job.preview_path.write_bytes(b'offline-image')
        self.job.attempts = [{'attempt': 1, 'provider': 'hunyuan', 'status': 'rejected',
                             'provider_error_category': 'validation',
                             'provider_error_ambiguous': False, 'error': 'original failure'}]
        return self.job

    def test_restart_restores_rejected_design_for_confirmation_without_submission(self):
        job = self.rejected_design()
        job.input_path = job.preview_path
        before = dict(job.attempts[0])
        sidecar._persist_job(job)
        with mock.patch.object(sidecar, '_validate_image_file'), \
             mock.patch.object(sidecar, '_submit') as submit:
            for _ in range(2):
                restored = sidecar._restore_jobs()
                self.assertEqual(len(restored), 1)
                self.assertEqual(restored[0].state, 'awaiting_confirmation')
                self.assertEqual(restored[0].phase, 'model_retry')
                self.assertEqual(restored[0].attempts, [before])
                persisted = json.loads((job.directory / 'job.json').read_text())
                self.assertEqual(persisted['state'], 'awaiting_confirmation')
                self.assertEqual(persisted['attempts'], [before])
            submit.assert_not_called()
        self.assertEqual(job.preview_path.read_bytes(), b'offline-image')

    def test_restart_keeps_ambiguous_rejection_failed(self):
        job = self.rejected_design()
        job.attempts[0]['provider_error_ambiguous'] = True
        sidecar._persist_job(job)
        with mock.patch.object(sidecar, '_validate_image_file'), \
             mock.patch.object(sidecar, '_submit') as submit:
            restored = sidecar._restore_jobs()
            self.assertEqual(restored[0].state, 'failed')
            submit.assert_not_called()

    def test_manual_retry_requires_confirmed_rejection_without_any_remote_task(self):
        job = self.rejected_design()
        self.assertTrue(sidecar._can_manually_retry_hunyuan(job))
        for change in ({'provider_error_ambiguous': True}, {'provider_error_ambiguous': None},
                       {'generation_task_id': 'paid-123'}, {'status': 'creating'},
                       {'provider_error_category': 'network'}, {'provider': 'tripo'}):
            with self.subTest(change=change), mock.patch.dict(job.attempts[0], change):
                self.assertFalse(sidecar._can_manually_retry_hunyuan(job))
        job.state = 'queued'
        self.assertFalse(sidecar._can_manually_retry_hunyuan(job))

    def test_confirmed_retry_has_new_authorization_and_retains_failed_attempt(self):
        job = self.rejected_design()
        before = dict(job.attempts[0])
        handler = object.__new__(sidecar.Handler)
        handler._read_model_json = mock.Mock(return_value={
            'prepared_prompt': '', 'provider': 'hunyuan', 'face_limit': 1000000,
            'geometry_quality': 'standard', 'texture_quality': 'standard', 'output_format': 'glb'})
        handler._get_job = mock.Mock(return_value=job)
        handler.send_json = mock.Mock()
        with mock.patch.object(sidecar, '_HUNYUAN_PROVIDER_GATEWAY') as hy, \
             mock.patch.object(sidecar, '_validate_image_file'), \
             mock.patch.object(sidecar, '_assess_reference_advice'), \
             mock.patch.object(sidecar, '_submit') as submit:
            hy.model_generation_available.return_value = True
            handler._generate(job.id)
            self.assertEqual(submit.call_args.args[-1].request_id, f'{job.id}:model:2')
            self.assertEqual(job.attempts, [before])
            self.assertEqual(job.state, 'queued')
            with self.assertRaises(sidecar.RequestError):
                handler._generate(job.id)
            submit.assert_called_once()

    def test_retry_worker_appends_failure_once_and_resume_uses_latest_task(self):
        job = self.rejected_design()
        before = dict(job.attempts[0])
        with mock.patch.object(sidecar, '_HUNYUAN_PROVIDER_GATEWAY') as hy, \
             mock.patch.object(sidecar, '_validate_image_file'), \
             mock.patch.object(sidecar, '_ensure_portrait_multiview', return_value=None):
            hy.start_or_reuse_model_task.side_effect = ProviderGatewayError(
                'image_base64 rejected; Request ID: request-2', code='InvalidParameter.InvalidParameter',
                category='validation', provider='hunyuan', operation='model_generation')
            authorization = PaidTaskAuthorization.confirmed(f'{job.id}:model:2', 'hunyuan')
            sidecar._generate_job(job, '', False, authorization)
            self.assertEqual(job.attempts[0], before)
            self.assertEqual(len(job.attempts), 2)
            self.assertEqual(job.attempts[1]['attempt'], 2)
            self.assertEqual(job.attempts[1]['provider_request_id'], f'{job.id}:model:2')
            self.assertIn('Request ID: request-2', job.attempts[1]['error'])
            hy.start_or_reuse_model_task.assert_called_once()
            hy.reset_mock()
            job.attempts[1].update(generation_task_id='paid-second', status='running')
            hy.start_or_reuse_model_task.side_effect = None
            hy.start_or_reuse_model_task.return_value = ProviderTaskRef('hunyuan', 'paid-second', True)
            hy.wait_for_task.side_effect = ProviderGatewayError('offline stop', code='provider_timeout', category='timeout')
            sidecar._generate_job(job, '', True)
            self.assertEqual(hy.start_or_reuse_model_task.call_args.kwargs['existing_task_id'], 'paid-second')
            self.assertIsNone(hy.start_or_reuse_model_task.call_args.kwargs['authorization'])
            self.assertEqual(job.attempts[0], before)

    def test_route_persists_provider_and_recovers_original_task(self):
        self.job.state = 'awaiting_confirmation'
        handler = object.__new__(sidecar.Handler)
        handler._read_model_json = mock.Mock(return_value={
            'prepared_prompt': 'test object', 'provider': 'hunyuan', 'face_limit': 300000,
            'geometry_quality': 'standard', 'texture_quality': 'standard', 'output_format': 'obj'})
        handler._get_job = mock.Mock(return_value=self.job)
        handler.send_json = mock.Mock()
        with mock.patch.object(sidecar, '_HUNYUAN_PROVIDER_GATEWAY') as hy, \
             mock.patch.object(sidecar, '_MODEL_PROVIDER_GATEWAY') as tripo, \
             mock.patch.object(sidecar, '_submit') as submit:
            hy.model_generation_available.return_value = True
            handler._generate(self.job.id)
            self.assertEqual(submit.call_args.args[-1].provider, 'hunyuan')
            tripo.model_generation_available.assert_not_called()
        self.job.attempts = [{'provider': 'hunyuan', 'generation_task_id': '12345', 'status': 'running'}]
        sidecar._persist_job(self.job)
        restored = sidecar._load_job(self.job.directory)
        self.assertEqual(restored.provider, 'hunyuan')
        self.assertEqual(sidecar._public_job(restored)['provider_tasks']['provider'], 'hunyuan')
        with mock.patch.object(sidecar, '_HUNYUAN_PROVIDER_GATEWAY') as hy, \
             mock.patch.object(sidecar, '_MODEL_PROVIDER_GATEWAY') as tripo:
            hy.start_or_reuse_model_task.return_value = ProviderTaskRef('hunyuan', '12345', True)
            hy.wait_for_task.side_effect = ProviderGatewayError('offline stop', code='provider_timeout', category='timeout')
            sidecar._generate_job(restored, 'test object', True)
            self.assertEqual(hy.start_or_reuse_model_task.call_args.kwargs['existing_task_id'], '12345')
            self.assertIsNone(hy.start_or_reuse_model_task.call_args.kwargs['authorization'])
            tripo.start_or_reuse_model_task.assert_not_called()

    def test_obj_download_never_submits_tripo_conversion(self):
        self.job.provider, self.job.output_format = 'hunyuan', 'obj'
        def download(_result, path, _limit, **options):
            self.assertEqual(options['output_format'], 'obj')
            path.write_bytes(b'PK offline zip placeholder')
        with mock.patch.object(sidecar, '_HUNYUAN_PROVIDER_GATEWAY') as hy, \
             mock.patch.object(sidecar, '_download_conversion') as conversion, \
             mock.patch.object(sidecar, '_prepare_obj_artifact', return_value=Path('model.obj')):
            hy.download_artifact.side_effect = download
            self.assertEqual(sidecar._download_generation_artifact(self.job, '12345'), Path('model.obj'))
            conversion.assert_not_called()

    def test_options_reject_tripo_only_settings(self):
        for options in ({'face_limit': 2000000}, {'geometry_quality': 'detailed'},
                        {'texture_quality': 'extreme'}, {'provider': 'unknown'}):
            payload = {'provider': 'hunyuan', **options}
            with self.assertRaises(sidecar.RequestError):
                sidecar._generation_options(payload, payload.get('face_limit', 1000000))

    def test_gateway_consumes_once_and_reuses_without_new_submission(self):
        gateway = gateway_module.HunyuanModelProviderGateway()
        authorization = PaidTaskAuthorization.confirmed('offline:1', 'hunyuan')
        request = ModelTaskRequest(source='text', prompt='object')
        with mock.patch.object(gateway_module.client, 'prepare_model_payload', return_value={}), \
             mock.patch.object(gateway_module.client, 'submit_model_task', return_value='12345') as submit:
            self.assertEqual(gateway.start_or_reuse_model_task(request, authorization=authorization).task_id, '12345')
            with self.assertRaises(ProviderGatewayError):
                gateway.start_or_reuse_model_task(request, authorization=authorization)
            self.assertTrue(gateway.start_or_reuse_model_task(request, existing_task_id='12345').reused)
            submit.assert_called_once()

    def test_legacy_state_defaults_to_tripo(self):
        path = self.job.directory / sidecar.JOB_STATE_FILENAME
        data = json.loads(path.read_text(encoding='utf-8'))
        data.pop('provider', None)
        path.write_text(json.dumps(data), encoding='utf-8')
        self.assertEqual(sidecar._load_job(self.job.directory).provider, 'tripo')

    def test_design_stage_persists_selected_provider_before_model_submission(self):
        handler = object.__new__(sidecar.Handler)
        handler._read_model_json = mock.Mock(return_value={
            'request_id': 'offline-design', 'prompt': 'test object', 'provider': 'hunyuan'})
        handler.send_json = mock.Mock()
        with mock.patch.object(sidecar, 'image_provider_status', return_value={'available': True}), \
             mock.patch.object(sidecar, '_submit') as submit:
            handler._create_text_job()
        job = submit.call_args.args[0]
        self.assertEqual(job.provider, 'hunyuan')
        self.assertEqual(sidecar._load_job(job.directory).provider, 'hunyuan')
        self.assertEqual(job.attempts, [])

    def test_temporary_query_failure_restarts_existing_task_without_submit(self):
        self.job.provider, self.job.state = 'hunyuan', 'failed'
        self.job.attempts = [{'provider': 'hunyuan', 'generation_task_id': '12345',
                             'provider_error_retryable': True, 'status': 'rejected'}]
        sidecar._persist_job(self.job)
        with mock.patch.object(sidecar, '_submit') as submit:
            restored = sidecar._restore_jobs(resume_jobs=False)
        self.assertEqual(restored[0].phase, 'resuming')
        self.assertEqual(restored[0].attempts[0]['generation_task_id'], '12345')
        submit.assert_not_called()

    def options_handler(self):
        handler = object.__new__(sidecar.Handler)
        handler._read_model_json = mock.Mock(return_value={
            'provider': 'hunyuan', 'face_limit': 300000, 'geometry_quality': 'standard',
            'texture_quality': 'standard', 'output_format': 'obj'})
        handler._get_job = mock.Mock(return_value=self.job)
        handler.send_json = mock.Mock()
        return handler

    def test_changing_provider_on_existing_design_survives_restart_without_generation(self):
        self.job.state = 'awaiting_confirmation'
        handler = self.options_handler()
        with mock.patch.object(sidecar, '_submit') as submit, \
             mock.patch.object(sidecar.PaidTaskAuthorization, 'confirmed') as authorization:
            handler._set_generation_options(self.job.id)
        restored = sidecar._load_job(self.job.directory)
        self.assertEqual((restored.provider, restored.face_limit, restored.output_format), ('hunyuan', 300000, 'obj'))
        self.assertEqual(restored.state, 'awaiting_confirmation')
        submit.assert_not_called()
        authorization.assert_not_called()

    def test_options_cannot_reassign_an_existing_paid_task(self):
        self.job.state = 'awaiting_confirmation'
        self.job.attempts = [{'generation_task_id': 'tripo-original'}]
        with self.assertRaises(sidecar.RequestError) as error:
            self.options_handler()._set_generation_options(self.job.id)
        self.assertEqual(error.exception.status, 409)
        self.assertEqual(self.job.provider, 'tripo')

    def test_options_save_failure_rolls_back_provider_and_parameters(self):
        self.job.state = 'awaiting_confirmation'
        self.job.face_limit = 1000000
        handler = self.options_handler()
        with mock.patch.object(sidecar, '_persist_job', side_effect=sidecar.TripoError('disk write failed')):
            with self.assertRaises(sidecar.RequestError) as error:
                handler._set_generation_options(self.job.id)
        self.assertEqual(error.exception.code, 'state_save_failed')
        self.assertEqual((self.job.provider, self.job.face_limit, self.job.output_format), ('tripo', 1000000, 'glb'))
        handler.send_json.assert_not_called()
