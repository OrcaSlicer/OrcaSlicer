from __future__ import annotations

import io
import json
import socket
import ssl
import unittest
import urllib.error
from unittest import mock

from tools.ai import ai_diagnostics


class AIDiagnosticsTests(unittest.TestCase):
    def test_safe_endpoint_removes_credentials_query_and_fragment(self) -> None:
        self.assertEqual(
            ai_diagnostics.safe_endpoint("https://user:secret@gateway.example:8443/openai/v1?api_key=hidden#part"),
            "https://gateway.example:8443/openai/v1",
        )

    def test_exception_details_preserves_network_reason_without_secrets(self) -> None:
        error = urllib.error.URLError(socket.gaierror(11001, "host failed token=very-secret"))
        details = ai_diagnostics.exception_details(error)
        self.assertEqual([item["type"] for item in details], ["URLError", "gaierror"])
        self.assertNotIn("very-secret", json.dumps(details))
        self.assertIn("<redacted>", json.dumps(details))

    def test_connection_failure_classification_distinguishes_common_causes(self) -> None:
        self.assertEqual(
            ai_diagnostics.classify_connection_error(urllib.error.URLError(socket.gaierror(11001, "missing"))),
            "dns",
        )
        self.assertEqual(
            ai_diagnostics.classify_connection_error(urllib.error.URLError(TimeoutError("timed out"))),
            "timeout",
        )
        self.assertEqual(
            ai_diagnostics.classify_connection_error(ssl.SSLCertVerificationError("certificate verify failed")),
            "tls_certificate",
        )
        self.assertEqual(
            ai_diagnostics.classify_connection_error(urllib.error.URLError(OSError("Tunnel connection failed"))),
            "proxy",
        )

    def test_event_includes_job_id_and_redacts_provider_data(self) -> None:
        stream = io.StringIO()
        with mock.patch.object(ai_diagnostics.sys, "stderr", stream):
            with ai_diagnostics.diagnostic_context("job-123"):
                ai_diagnostics.event(
                    "provider.connection.failed",
                    endpoint=ai_diagnostics.safe_endpoint("https://user:pw@example.test/v1?token=secret"),
                    detail="Authorization: Bearer sk-super-secret-value",
                )
        payload = json.loads(stream.getvalue())
        self.assertEqual(payload["job_id"], "job-123")
        self.assertEqual(payload["endpoint"], "https://example.test/v1")
        self.assertNotIn("super-secret", stream.getvalue())
        self.assertIn("<redacted>", payload["detail"])

    def test_event_redacts_top_level_and_nested_sensitive_fields(self) -> None:
        stream = io.StringIO()
        secrets = {
            "api_key": "api-key-value",
            "OPENAI_PRO_API": "pro-api-value",
            "HY3D_API": "hy-test-value",
            "accessToken": "access-token-value",
            "client_secret": "client-secret-value",
            "password": "password-value",
            "Authorization": "Bearer authorization-value",
            "cookie_header": "session=cookie-value",
            "x_orcaslicer_session_proof": "proof-value",
            "clientNonce": "client-nonce-value",
            "server_nonce": "server-nonce-value",
        }
        with mock.patch.object(ai_diagnostics.sys, "stderr", stream):
            ai_diagnostics.event(
                "diagnostics.redaction",
                api_key="top-level-api-key",
                nested={"provider": secrets, "items": [{"refresh_token": "refresh-token-value"}]},
                token_count=42,
                nonce_length=64,
                password_policy="managed",
            )

        payload = json.loads(stream.getvalue())
        self.assertEqual(payload["api_key"], "<redacted>")
        self.assertEqual(payload["nested"]["provider"], {key: "<redacted>" for key in secrets})
        self.assertEqual(payload["nested"]["items"][0]["refresh_token"], "<redacted>")
        self.assertEqual(payload["token_count"], 42)
        self.assertEqual(payload["nonce_length"], 64)
        self.assertEqual(payload["password_policy"], "managed")
        for secret in ["top-level-api-key", *secrets.values(), "refresh-token-value"]:
            self.assertNotIn(secret, stream.getvalue())

    def test_redact_text_sanitizes_valid_and_embedded_json(self) -> None:
        raw_json = json.dumps(
            {
                "safe": "visible",
                "auth": {
                    "TRIPO_API_KEY": "json-api-key",
                    "sessionProof": ["proof-one", "proof-two"],
                    "serverNonce": 123456,
                },
            }
        )
        payload = json.loads(ai_diagnostics.redact_text(raw_json))
        self.assertEqual(payload["safe"], "visible")
        self.assertEqual(payload["auth"]["TRIPO_API_KEY"], "<redacted>")
        self.assertEqual(payload["auth"]["sessionProof"], "<redacted>")
        self.assertEqual(payload["auth"]["serverNonce"], "<redacted>")

        embedded = "provider failed: {'api_key':'embedded-key','password':'embedded-password'}"
        redacted = ai_diagnostics.redact_text(embedded)
        self.assertNotIn("embedded-key", redacted)
        self.assertNotIn("embedded-password", redacted)
        self.assertEqual(redacted.count("<redacted>"), 2)

        pro_assignment = ai_diagnostics.redact_text("OPENAI_PRO_API=pro-assignment-secret")
        self.assertNotIn("pro-assignment-secret", pro_assignment)
        self.assertIn("<redacted>", pro_assignment)

    def test_exception_details_redacts_json_encoded_credentials(self) -> None:
        error = RuntimeError(
            'provider response={"authorization":"Basic encoded-value",'
            '"cookie":"sid=cookie-value","session_proof":"proof-value"}'
        )
        serialized = json.dumps(ai_diagnostics.exception_details(error))
        self.assertNotIn("encoded-value", serialized)
        self.assertNotIn("cookie-value", serialized)
        self.assertNotIn("proof-value", serialized)
        self.assertEqual(serialized.count("<redacted>"), 3)

    def test_event_write_failure_is_non_fatal(self) -> None:
        with mock.patch.object(ai_diagnostics.sys, "stderr", None):
            ai_diagnostics.event("diagnostics.unavailable", value="safe")


if __name__ == "__main__":
    unittest.main()
