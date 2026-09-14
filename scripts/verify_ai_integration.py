#!/usr/bin/env python3
"""Validate the pinned OrcaSlicer AI integration contract.

The lock file is intentionally machine-readable and contains only public Git
identities and runtime metadata. It must never contain provider credentials.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
from typing import Any, Iterable, Sequence


SCHEMA = "orcaslicer.ai-integration-lock/v4"
EXPECTED_INTEGRATION_BRANCH = "codex/team/integration"
EXPECTED_BRANCH_POLICY = {
    "long_lived_branches": {
        "model_generation": "codex/team/model-generation",
        "smart_slicing": "codex/team/smart-slicing",
        "maintenance": "codex/team/maintenance",
        "integration": "codex/team/integration",
    },
    "developer_sync_source": "codex/team/integration",
    "long_lived_sync_method": "merge_commit",
    "force_push_allowed": False,
    "integration_requires_pull_request": True,
    "integration_requires_latest_base_and_head": True,
    "historical_provenance": "fixed_source_sha_and_integration_receipt",
}
EXPECTED_CI_CONTRACT = {
    "guardrails_workflow": ".github/workflows/ai-integration-guardrails.yml",
    "candidate_workflow": ".github/workflows/team-integration-candidate.yml",
    "required_checks": ["AI integration checks", "Team integration candidate"],
    "merge_queue_event": "merge_group",
    "path_filters_allowed": False,
}
EXPECTED_UPSTREAM = {
    "remote": "upstream",
    "branch": "main",
    "ref": "upstream/main",
    "sha": "b6ef6cf1be428b931d669dcc31b7f43ae320be85",
}
EXPECTED_FEATURE_SOURCES = {
    "model_generation": {
        "branch": "codex/model-generation",
        "sha": "16f8ac0fb048570edb5fe787bdfdfcde4c9fa4b7",
    },
    "smart_slicing": {
        "branch": "codex/smart-slicing",
        "sha": "1c163d68906e287b946b40a975feb3bfd9aab68d",
    },
}
EXPECTED_INTEGRATION_RECEIPTS = {
    "model_generation": {
        "source_sha": "16f8ac0fb048570edb5fe787bdfdfcde4c9fa4b7",
        "integration_commit": "616a063192fb0e1ff52ecaf34d4a4c8ee8fae271",
        "method": "verified_git_object_subset",
        "verified_git_objects": {
            "src/slic3r/GUI/AIModelGenerationClient.hpp": "65bda77f5e98d0e8f3dbb28951839fb15289c60c",
            "tools/ai/image2_quality_benchmark.py": "178102bfdc99c991625b5157264a472d5def65e7",
            "tools/ai/model_input_image_quality.py": "bfa3a2f05e58fa5e56b7145ea2a5958aec3e2ffd",
            "tools/ai/model_provider_gateway.py": "f5cf429f4f0b3ff59b0c6de09149f154f460b7da",
            "tools/ai/portrait_multiview_cleanup.py": "dce58f808452e73c7a4737fa748e6f49557cc8f9",
            "tools/ai/printable_image_pipeline.py": "04453961425d7c101bd1b734738c4e02e5c5e68c",
            "tools/ai/printable_multiview_reference.py": "495a81426280ea80805792e86b86194bae6b5b9b",
            "tools/ai/printable_reference_visual_quality.py": "4c90d2c4ceed52698b98788414fcbd2ce6633a72",
            "tools/ai/test_printable_sidecar_pipeline.py": "40e8d7ad7ae13818f761541a9f8367ea1623b005",
        },
    },
    "smart_slicing": {
        "source_sha": "1c163d68906e287b946b40a975feb3bfd9aab68d",
        "integration_commit": "c1cdfece8752d24637ec9d62edc70f2101455b1f",
        "method": "verified_git_object_subset",
        "verified_git_objects": {
            "src/slic3r/AI/SmartSlicing": "1dce995947edfaf834388f25d278148dfe9f1cf9",
            "src/slic3r/GUI/AI/SmartSlicing": "c6afa49cba2e5b107fa1430c249cebd9e9371d1a",
        },
    },
}
EXPECTED_PROTOCOL_VERSION = 2
EXPECTED_SIDECAR_VERSION = "orcaslicer-ai-sidecar-v9"
EXPECTED_PRODUCT_PORT = 18764
EXPECTED_DEVELOPMENT_PORTS = {
    "model_generation": 18765,
    "smart_slicing": 18766,
    "integration": 18767,
}
EXPECTED_ARCHITECTURE = {
    "pattern": "desktop_modular_monolith",
    "migration_phase": "gui_feature_hosts",
    "target_contract_root": "src/slic3r/AI/Contracts",
}
EXPECTED_FEATURE_HOSTS = {
    "desktop": "src/slic3r/GUI/AI/AIDesktopFeatureHost.hpp",
    "model_generation": "src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.hpp",
    "smart_slicing": "src/slic3r/GUI/AI/SmartSlicing/SmartSlicingFeatureHost.hpp",
}
EXPECTED_COMPOSITION_ROOTS = {
    "CMakeLists.txt",
    "src/CMakeLists.txt",
    "src/slic3r/CMakeLists.txt",
    "src/slic3r/GUI/MainFrame.cpp",
    "src/slic3r/GUI/MainFrame.hpp",
    "src/slic3r/GUI/Plater.cpp",
    "src/slic3r/GUI/Plater.hpp",
}
EXPECTED_DECOMPOSITION_TARGETS = {
    "src/slic3r/GUI/ModelGenerationPanel.cpp",
    "tools/ai/orca_ai_sidecar.py",
}
EXPECTED_RELEASE_PROMOTION = {
    "internal_fast_promotable": False,
    "commercial_candidate_requires_exact_sha": True,
    "production_promotes_candidate_artifact": True,
    "production_rebuild_allowed": False,
}
EXPECTED_BOUNDARY_FLAGS = (
    "integration_consumes_exact_accepted_sha",
    "preserve_orca_defaults",
    "preserve_3mf_and_profile_formats",
    "paid_api_requires_explicit_authorization",
)
REQUIRED_INTEGRATION_PATHS = {
    ".github",
    "CMakeLists.txt",
    "build_release_vs.bat",
    "deps/MPFR/MPFR.cmake",
    "deps/wxWidgets/wxWidgets.cmake",
    "docs/architecture",
    "scripts/package_internal_fast.ps1",
    "scripts/verify_ai_integration.py",
    "src/CMakeLists.txt",
    "src/slic3r/AI/CMakeLists.txt",
    "src/slic3r/AI/Contracts",
    "src/slic3r/CMakeLists.txt",
    "src/slic3r/GUI/AI/AIDesktopFeatureHost.cpp",
    "src/slic3r/GUI/AI/AIDesktopFeatureHost.hpp",
    "src/slic3r/GUI/AI/Orca",
    "src/slic3r/GUI/MainFrame.cpp",
    "src/slic3r/GUI/MainFrame.hpp",
    "src/slic3r/GUI/Plater.cpp",
    "src/slic3r/GUI/Plater.hpp",
}
REQUIRED_SHARED_RUNTIME_PATHS = {
    "src/slic3r/GUI/AIServiceManager.cpp",
    "src/slic3r/GUI/AIServiceManager.hpp",
    "src/slic3r/GUI/AISidecarClient.cpp",
    "src/slic3r/GUI/AISidecarClient.hpp",
    "src/slic3r/Utils/Http.cpp",
    "src/slic3r/Utils/Http.hpp",
    "tools/ai/ai_diagnostics.py",
    "tools/ai/network_policy.py",
    "tools/ai/orca_ai_build_info.json.in",
    "tools/ai/orca_ai_installed_bootstrap.py",
    "tools/ai/orca_ai_runtime_dependencies.json.in",
    "tools/ai/verify_bundled_runtime.py",
}
REQUIRED_MODEL_GENERATION_PATHS = {
    "src/slic3r/AI/ModelGeneration",
    "src/slic3r/GUI/AI/ModelGeneration",
    "src/slic3r/GUI/AI/Model/ModelFinishing.cpp",
    "src/slic3r/GUI/AI/Model/ModelFinishing.hpp",
}
REQUIRED_CONTRACT_PATHS = {
    "src/slic3r/AI/Contracts/GeneratedModelArtifact.hpp",
    "src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp",
    "src/slic3r/AI/Contracts/IPrintablePaletteProvider.hpp",
    "src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp",
}
NEUTRAL_CONTRACT_HEADERS = (
    "src/slic3r/AI/Contracts/GeneratedModelArtifact.hpp",
    "src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp",
    "src/slic3r/AI/Contracts/IPrintablePaletteProvider.hpp",
)
LEGACY_CONTRACT_FORWARDERS = {
    "src/slic3r/AI/ModelGeneration/GeneratedModelArtifact.hpp": (
        '#pragma once\n\n#include "slic3r/AI/Contracts/GeneratedModelArtifact.hpp"\n'
    ),
    "src/slic3r/AI/ModelGeneration/IPrintablePaletteProvider.hpp": (
        '#pragma once\n\n#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"\n'
    ),
    "src/slic3r/AI/SmartSlicing/IModelArtifactConsumer.hpp": (
        '#pragma once\n\n#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"\n'
    ),
}
DIRECT_CONTRACT_CONSUMERS = {
    "src/slic3r/GUI/ModelGenerationPanel.hpp": (
        '#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"',
        '#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"',
    ),
    "src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp": (
        '#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"',
        '#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"',
    ),
}
SMART_SLICING_CORE_SOURCES = (
    "SmartSlicing/Application/SmartSlicingCoordinator.cpp",
    "SmartSlicing/Application/PrintabilityInspector.cpp",
    "SmartSlicing/Domain/CandidateComparison.cpp",
    "SmartSlicing/Domain/ParameterProposalValidator.cpp",
)
GUI_FEATURE_FILES = (
    "src/slic3r/GUI/AI/AIDesktopFeatureHost.cpp",
    "src/slic3r/GUI/AI/AIDesktopFeatureHost.hpp",
    "src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.cpp",
    "src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.hpp",
    "src/slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.cpp",
    "src/slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.hpp",
    "src/slic3r/GUI/AI/ModelGeneration/ModelPreview3D.hpp",
    "src/slic3r/GUI/AI/SmartSlicing/SmartSlicingFeatureHost.cpp",
    "src/slic3r/GUI/AI/SmartSlicing/SmartSlicingFeatureHost.hpp",
)
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
FORBIDDEN_TRACKED_SECRET_NAMES = {
    "orca_ai_internal_defaults.json",
}
SECRET_SCAN_SUFFIXES = {
    ".bat", ".cmake", ".cpp", ".h", ".hpp", ".json", ".md", ".ps1", ".py", ".txt", ".yaml", ".yml",
}
DIRECT_OPENAI_KEY_PATTERN = re.compile(r"\bsk-(?:proj-)?[A-Za-z0-9_-]{30,}\b")
LITERAL_PROVIDER_KEY_PATTERN = re.compile(
    r'''(?i)["']?(HY3D_API|OPENAI_PRO_API|OPENAI_API_KEY|TRIPO_API_KEY)["']?\s*[:=]\s*["']([^"'\r\n]{16,})["']'''
)
PRIVATE_KEY_PATTERN = re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")
# These two audited negative-test payloads deliberately resemble credentials.
# Match the exact file, variable and value; never exempt a test file or a prefix.
AUDITED_CREDENTIAL_FIXTURES = {
    ("release/test_verify_package_contents.py", "TRIPO_API_KEY", "fixture-opaque-value"),
    ("tools/ai/test_diagnostic_failure_flow.py", "TRIPO_API_KEY", "unused-tripo-key"),
}


def _issue(code: str, message: str) -> dict[str, str]:
    return {"code": code, "message": message}


def _expect_object(value: Any, name: str, errors: list[dict[str, str]]) -> dict[str, Any]:
    if not isinstance(value, dict):
        errors.append(_issue("schema.type", f"{name} must be an object"))
        return {}
    return value


def _expect_exact_keys(
    value: dict[str, Any], expected: Iterable[str], name: str, errors: list[dict[str, str]]
) -> None:
    expected_keys = set(expected)
    actual_keys = set(value)
    missing = sorted(expected_keys - actual_keys)
    unknown = sorted(actual_keys - expected_keys)
    if missing:
        errors.append(_issue("schema.missing", f"{name} is missing: {', '.join(missing)}"))
    if unknown:
        errors.append(_issue("schema.unknown", f"{name} has unknown fields: {', '.join(unknown)}"))


def _expect_constant(
    actual: Any, expected: Any, name: str, errors: list[dict[str, str]]
) -> None:
    if actual != expected:
        errors.append(_issue("contract.constant", f"{name} must be {expected!r}, got {actual!r}"))


def _validate_sha(value: Any, name: str, errors: list[dict[str, str]]) -> None:
    if not isinstance(value, str) or SHA_PATTERN.fullmatch(value) is None:
        errors.append(_issue("schema.sha", f"{name} must be a lowercase full 40-character Git SHA"))


def _validate_path_list(value: Any, name: str, errors: list[dict[str, str]]) -> set[str]:
    if not isinstance(value, list) or not value:
        errors.append(_issue("boundary.paths", f"{name} must be a non-empty array"))
        return set()

    valid_paths: set[str] = set()
    for index, item in enumerate(value):
        if not isinstance(item, str) or not item:
            errors.append(_issue("boundary.path", f"{name}[{index}] must be a non-empty string"))
            continue
        path = PurePosixPath(item)
        if item.startswith("/") or "\\" in item or ".." in path.parts or str(path) != item:
            errors.append(
                _issue("boundary.path", f"{name}[{index}] must be a normalized repository-relative POSIX path")
            )
            continue
        if item in valid_paths:
            errors.append(_issue("boundary.duplicate", f"{name} contains duplicate path {item!r}"))
            continue
        valid_paths.add(item)
    return valid_paths


def _overlapping_paths(first: Iterable[str], second: Iterable[str]) -> list[tuple[str, str]]:
    """Return exact or parent/child overlaps between two ownership sets."""
    overlaps: list[tuple[str, str]] = []
    for left in sorted(first):
        left_parts = PurePosixPath(left).parts
        for right in sorted(second):
            right_parts = PurePosixPath(right).parts
            common_length = min(len(left_parts), len(right_parts))
            if left_parts[:common_length] == right_parts[:common_length]:
                overlaps.append((left, right))
    return overlaps


def validate_document(document: Any) -> list[dict[str, str]]:
    """Validate schema, pinned identities and ownership boundary declarations."""
    errors: list[dict[str, str]] = []
    root = _expect_object(document, "document", errors)
    _expect_exact_keys(
        root,
        (
            "schema",
            "integration_branch",
            "branch_policy",
            "ci_contract",
            "upstream",
            "feature_sources",
            "integration_receipts",
            "runtime_contract",
            "architecture_contract",
            "boundaries",
        ),
        "document",
        errors,
    )
    _expect_constant(root.get("schema"), SCHEMA, "schema", errors)
    _expect_constant(root.get("integration_branch"), EXPECTED_INTEGRATION_BRANCH, "integration_branch", errors)

    branch_policy = _expect_object(root.get("branch_policy"), "branch_policy", errors)
    _expect_exact_keys(branch_policy, EXPECTED_BRANCH_POLICY, "branch_policy", errors)
    for key, expected in EXPECTED_BRANCH_POLICY.items():
        _expect_constant(branch_policy.get(key), expected, f"branch_policy.{key}", errors)

    ci_contract = _expect_object(root.get("ci_contract"), "ci_contract", errors)
    _expect_exact_keys(ci_contract, EXPECTED_CI_CONTRACT, "ci_contract", errors)
    for key, expected in EXPECTED_CI_CONTRACT.items():
        _expect_constant(ci_contract.get(key), expected, f"ci_contract.{key}", errors)

    upstream = _expect_object(root.get("upstream"), "upstream", errors)
    _expect_exact_keys(upstream, EXPECTED_UPSTREAM, "upstream", errors)
    for key, expected in EXPECTED_UPSTREAM.items():
        _expect_constant(upstream.get(key), expected, f"upstream.{key}", errors)
    _validate_sha(upstream.get("sha"), "upstream.sha", errors)

    sources = _expect_object(root.get("feature_sources"), "feature_sources", errors)
    _expect_exact_keys(sources, EXPECTED_FEATURE_SOURCES, "feature_sources", errors)
    for source_name, expected_source in EXPECTED_FEATURE_SOURCES.items():
        source = _expect_object(sources.get(source_name), f"feature_sources.{source_name}", errors)
        _expect_exact_keys(source, expected_source, f"feature_sources.{source_name}", errors)
        for key, expected in expected_source.items():
            _expect_constant(source.get(key), expected, f"feature_sources.{source_name}.{key}", errors)
        _validate_sha(source.get("sha"), f"feature_sources.{source_name}.sha", errors)

    receipts = _expect_object(root.get("integration_receipts"), "integration_receipts", errors)
    _expect_exact_keys(receipts, EXPECTED_INTEGRATION_RECEIPTS, "integration_receipts", errors)
    for feature_name, expected_receipt in EXPECTED_INTEGRATION_RECEIPTS.items():
        receipt = _expect_object(receipts.get(feature_name), f"integration_receipts.{feature_name}", errors)
        _expect_exact_keys(receipt, expected_receipt, f"integration_receipts.{feature_name}", errors)
        for key in ("source_sha", "integration_commit", "method"):
            _expect_constant(
                receipt.get(key),
                expected_receipt[key],
                f"integration_receipts.{feature_name}.{key}",
                errors,
            )
        _validate_sha(receipt.get("source_sha"), f"integration_receipts.{feature_name}.source_sha", errors)
        _validate_sha(
            receipt.get("integration_commit"),
            f"integration_receipts.{feature_name}.integration_commit",
            errors,
        )
        objects = _expect_object(
            receipt.get("verified_git_objects"),
            f"integration_receipts.{feature_name}.verified_git_objects",
            errors,
        )
        expected_objects = expected_receipt["verified_git_objects"]
        _expect_exact_keys(
            objects,
            expected_objects,
            f"integration_receipts.{feature_name}.verified_git_objects",
            errors,
        )
        for path, expected_object in expected_objects.items():
            _expect_constant(
                objects.get(path),
                expected_object,
                f"integration_receipts.{feature_name}.verified_git_objects.{path}",
                errors,
            )
            _validate_sha(
                objects.get(path),
                f"integration_receipts.{feature_name}.verified_git_objects.{path}",
                errors,
            )

    runtime = _expect_object(root.get("runtime_contract"), "runtime_contract", errors)
    _expect_exact_keys(
        runtime,
        ("protocol_version", "sidecar_version", "product_port", "development_ports", "development_port_policy"),
        "runtime_contract",
        errors,
    )
    _expect_constant(runtime.get("protocol_version"), EXPECTED_PROTOCOL_VERSION, "runtime_contract.protocol_version", errors)
    _expect_constant(runtime.get("sidecar_version"), EXPECTED_SIDECAR_VERSION, "runtime_contract.sidecar_version", errors)
    _expect_constant(runtime.get("product_port"), EXPECTED_PRODUCT_PORT, "runtime_contract.product_port", errors)
    development_ports = _expect_object(runtime.get("development_ports"), "runtime_contract.development_ports", errors)
    _expect_exact_keys(development_ports, EXPECTED_DEVELOPMENT_PORTS, "runtime_contract.development_ports", errors)
    for role, expected_port in EXPECTED_DEVELOPMENT_PORTS.items():
        _expect_constant(development_ports.get(role), expected_port, f"runtime_contract.development_ports.{role}", errors)
    development_port_values = list(development_ports.values())
    if not all(type(value) is int and 1 <= value <= 65535 for value in development_port_values):
        errors.append(_issue("runtime.ports", "development ports must be integers between 1 and 65535"))
    else:
        if len(set(development_port_values)) != len(development_port_values):
            errors.append(_issue("runtime.ports", "development ports must be unique per development role"))
        if runtime.get("product_port") in development_port_values:
            errors.append(_issue("runtime.product_port", "product_port must not be assigned to a development role"))
    policy = runtime.get("development_port_policy")
    if not isinstance(policy, str) or "explicit runtime override" not in policy or "product_port" not in policy:
        errors.append(
            _issue("runtime.port_policy", "development_port_policy must distinguish explicit overrides from product_port")
        )

    architecture = _expect_object(root.get("architecture_contract"), "architecture_contract", errors)
    _expect_exact_keys(
        architecture,
        (
            *EXPECTED_ARCHITECTURE,
            "composition_roots",
            "feature_hosts",
            "decomposition_line_budgets",
            "shared_touchpoint_diff_budgets",
            "release_promotion",
        ),
        "architecture_contract",
        errors,
    )
    for key, expected in EXPECTED_ARCHITECTURE.items():
        _expect_constant(architecture.get(key), expected, f"architecture_contract.{key}", errors)

    feature_hosts = _expect_object(architecture.get("feature_hosts"), "architecture_contract.feature_hosts", errors)
    _expect_exact_keys(feature_hosts, EXPECTED_FEATURE_HOSTS, "architecture_contract.feature_hosts", errors)
    for key, expected in EXPECTED_FEATURE_HOSTS.items():
        _expect_constant(feature_hosts.get(key), expected, f"architecture_contract.feature_hosts.{key}", errors)

    composition_roots = _validate_path_list(
        architecture.get("composition_roots"), "architecture_contract.composition_roots", errors
    )
    if composition_roots != EXPECTED_COMPOSITION_ROOTS:
        missing = sorted(EXPECTED_COMPOSITION_ROOTS - composition_roots)
        unknown = sorted(composition_roots - EXPECTED_COMPOSITION_ROOTS)
        if missing:
            errors.append(
                _issue("architecture.composition_roots", f"composition roots are missing: {', '.join(missing)}")
            )
        if unknown:
            errors.append(
                _issue("architecture.composition_roots", f"unknown composition roots: {', '.join(unknown)}")
            )

    line_budgets = _expect_object(
        architecture.get("decomposition_line_budgets"),
        "architecture_contract.decomposition_line_budgets",
        errors,
    )
    line_budget_paths = _validate_path_list(
        list(line_budgets), "architecture_contract.decomposition_line_budgets paths", errors
    )
    if line_budget_paths != EXPECTED_DECOMPOSITION_TARGETS:
        missing = sorted(EXPECTED_DECOMPOSITION_TARGETS - line_budget_paths)
        unknown = sorted(line_budget_paths - EXPECTED_DECOMPOSITION_TARGETS)
        if missing:
            errors.append(
                _issue("architecture.line_budget", f"decomposition budgets are missing: {', '.join(missing)}")
            )
        if unknown:
            errors.append(
                _issue("architecture.line_budget", f"unknown decomposition budgets: {', '.join(unknown)}")
            )
    for path, maximum in line_budgets.items():
        if type(maximum) is not int or maximum <= 0:
            errors.append(
                _issue("architecture.line_budget", f"decomposition line budget for {path} must be a positive integer")
            )

    diff_budgets = _expect_object(
        architecture.get("shared_touchpoint_diff_budgets"),
        "architecture_contract.shared_touchpoint_diff_budgets",
        errors,
    )
    diff_budget_paths = _validate_path_list(
        list(diff_budgets), "architecture_contract.shared_touchpoint_diff_budgets paths", errors
    )
    if diff_budget_paths != composition_roots:
        errors.append(
            _issue(
                "architecture.diff_budget",
                "shared touchpoint diff budgets must cover exactly the declared composition roots",
            )
        )
    for path, value in diff_budgets.items():
        budget = _expect_object(value, f"architecture_contract.shared_touchpoint_diff_budgets.{path}", errors)
        _expect_exact_keys(
            budget,
            ("max_added_lines", "max_net_added_lines"),
            f"architecture_contract.shared_touchpoint_diff_budgets.{path}",
            errors,
        )
        for field in ("max_added_lines", "max_net_added_lines"):
            maximum = budget.get(field)
            if type(maximum) is not int or maximum < 0:
                errors.append(
                    _issue("architecture.diff_budget", f"{path} {field} must be a non-negative integer")
                )

    promotion = _expect_object(architecture.get("release_promotion"), "architecture_contract.release_promotion", errors)
    _expect_exact_keys(promotion, EXPECTED_RELEASE_PROMOTION, "architecture_contract.release_promotion", errors)
    for key, expected in EXPECTED_RELEASE_PROMOTION.items():
        _expect_constant(promotion.get(key), expected, f"architecture_contract.release_promotion.{key}", errors)

    boundaries = _expect_object(root.get("boundaries"), "boundaries", errors)
    _expect_exact_keys(
        boundaries,
        (
            *EXPECTED_BOUNDARY_FLAGS,
            "integration_owned_paths",
            "shared_runtime_owned_paths",
            "model_generation_owned_paths",
            "smart_slicing_owned_paths",
            "allowed_cross_feature_contracts",
        ),
        "boundaries",
        errors,
    )
    for flag in EXPECTED_BOUNDARY_FLAGS:
        if boundaries.get(flag) is not True:
            errors.append(_issue("boundary.rule", f"boundaries.{flag} must be true"))

    integration_paths = _validate_path_list(
        boundaries.get("integration_owned_paths"), "boundaries.integration_owned_paths", errors
    )
    shared_runtime_paths = _validate_path_list(
        boundaries.get("shared_runtime_owned_paths"), "boundaries.shared_runtime_owned_paths", errors
    )
    model_paths = _validate_path_list(
        boundaries.get("model_generation_owned_paths"), "boundaries.model_generation_owned_paths", errors
    )
    slicing_paths = _validate_path_list(
        boundaries.get("smart_slicing_owned_paths"), "boundaries.smart_slicing_owned_paths", errors
    )
    contract_paths = _validate_path_list(
        boundaries.get("allowed_cross_feature_contracts"), "boundaries.allowed_cross_feature_contracts", errors
    )
    missing_integration = sorted(REQUIRED_INTEGRATION_PATHS - integration_paths)
    if missing_integration:
        errors.append(
            _issue("boundary.integration", f"integration ownership is missing: {', '.join(missing_integration)}")
        )
    target_contract_root = architecture.get("target_contract_root")
    if target_contract_root not in integration_paths:
        errors.append(
            _issue("boundary.integration", "the target neutral contract root must be integration-owned")
        )
    missing_shared_runtime = sorted(REQUIRED_SHARED_RUNTIME_PATHS - shared_runtime_paths)
    if missing_shared_runtime:
        errors.append(
            _issue(
                "boundary.shared_runtime",
                f"shared runtime ownership is missing: {', '.join(missing_shared_runtime)}",
            )
        )
    missing_model_generation = sorted(REQUIRED_MODEL_GENERATION_PATHS - model_paths)
    if missing_model_generation:
        errors.append(
            _issue(
                "boundary.model_generation",
                f"model generation ownership is missing: {', '.join(missing_model_generation)}",
            )
        )
    missing_contracts = sorted(REQUIRED_CONTRACT_PATHS - contract_paths)
    if missing_contracts:
        errors.append(_issue("boundary.contract", f"allowed contracts are missing: {', '.join(missing_contracts)}"))
    ownership_groups = {
        "integration": integration_paths,
        "shared_runtime": shared_runtime_paths,
        "model_generation": model_paths,
        "smart_slicing": slicing_paths,
    }
    group_names = tuple(ownership_groups)
    for first_index, first_name in enumerate(group_names):
        for second_name in group_names[first_index + 1 :]:
            overlaps = _overlapping_paths(ownership_groups[first_name], ownership_groups[second_name])
            if overlaps:
                rendered = ", ".join(f"{left} <> {right}" for left, right in overlaps)
                errors.append(
                    _issue(
                        "boundary.overlap",
                        f"{first_name} and {second_name} ownership overlap: {rendered}",
                    )
                )
    return errors


def _run_git(repo_root: Path, arguments: Sequence[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(repo_root), *arguments],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def validate_architecture_budgets(document: dict[str, Any], repo_root: Path) -> list[dict[str, str]]:
    """Prevent designated decomposition targets and Orca composition roots from growing silently."""
    errors: list[dict[str, str]] = []
    architecture = document.get("architecture_contract")
    if not isinstance(architecture, dict):
        return [_issue("architecture.contract", "architecture_contract must be an object")]

    line_budgets = architecture.get("decomposition_line_budgets")
    if not isinstance(line_budgets, dict):
        errors.append(_issue("architecture.line_budget", "decomposition_line_budgets must be an object"))
    else:
        for relative_path, maximum in line_budgets.items():
            if not isinstance(relative_path, str) or type(maximum) is not int or maximum <= 0:
                errors.append(_issue("architecture.line_budget", "decomposition line budgets are malformed"))
                continue
            path = repo_root / relative_path
            try:
                line_count = len(path.read_text(encoding="utf-8", errors="replace").splitlines())
            except OSError:
                errors.append(_issue("architecture.missing_target", f"decomposition target is missing: {relative_path}"))
                continue
            if line_count > maximum:
                errors.append(
                    _issue(
                        "architecture.line_budget",
                        f"{relative_path} has {line_count} lines, exceeding its architecture budget of {maximum}",
                    )
                )

    diff_budgets = architecture.get("shared_touchpoint_diff_budgets")
    if not isinstance(diff_budgets, dict):
        errors.append(_issue("architecture.diff_budget", "shared_touchpoint_diff_budgets must be an object"))
        return errors
    if not diff_budgets:
        return errors

    upstream = document.get("upstream")
    upstream_sha = upstream.get("sha") if isinstance(upstream, dict) else None
    if not isinstance(upstream_sha, str) or SHA_PATTERN.fullmatch(upstream_sha) is None:
        errors.append(_issue("architecture.diff_base", "architecture diff budget requires a full upstream SHA"))
        return errors

    relative_paths = tuple(sorted(path for path in diff_budgets if isinstance(path, str)))
    result = _run_git(
        repo_root,
        ["diff", "--no-renames", "--numstat", upstream_sha, "--", *relative_paths],
    )
    if result.returncode != 0:
        errors.append(_issue("architecture.diff_failed", "shared touchpoint diff could not be measured"))
        return errors

    actual: dict[str, tuple[int, int]] = {}
    for record in result.stdout.splitlines():
        parts = record.split("\t", 2)
        if len(parts) != 3:
            errors.append(_issue("architecture.diff_failed", "shared touchpoint numstat output is malformed"))
            continue
        added_text, deleted_text, relative_path = parts
        if not added_text.isdigit() or not deleted_text.isdigit():
            errors.append(_issue("architecture.diff_binary", f"binary composition root is not allowed: {relative_path}"))
            continue
        actual[relative_path] = (int(added_text), int(deleted_text))

    for relative_path, value in diff_budgets.items():
        if not isinstance(value, dict):
            continue
        maximum_added = value.get("max_added_lines")
        maximum_net_added = value.get("max_net_added_lines")
        if type(maximum_added) is not int or type(maximum_net_added) is not int:
            continue
        added, deleted = actual.get(relative_path, (0, 0))
        net_added = added - deleted
        if added > maximum_added or net_added > maximum_net_added:
            errors.append(
                _issue(
                    "architecture.diff_budget",
                    f"{relative_path} diff is +{added}/-{deleted} (net {net_added}), "
                    f"exceeding budgets +{maximum_added}/net {maximum_net_added}",
                )
            )
    return errors


def _git_object_at_path(repo_root: Path, commit: str, path: str) -> str | None:
    result = _run_git(repo_root, ["rev-parse", f"{commit}:{path}"])
    if result.returncode != 0:
        return None
    value = result.stdout.strip()
    return value if SHA_PATTERN.fullmatch(value) else None


def validate_tracked_paths(paths: Iterable[str]) -> list[dict[str, str]]:
    """Reject package-only credential payloads from Git."""
    errors: list[dict[str, str]] = []
    for value in paths:
        normalized = value.replace("\\", "/")
        name = PurePosixPath(normalized).name.lower()
        if name in FORBIDDEN_TRACKED_SECRET_NAMES or name == ".env" or name.startswith(".env."):
            errors.append(_issue("security.tracked_secret", f"secret-bearing file must not be tracked: {normalized}"))
    return errors


def _placeholder_secret(value: str) -> bool:
    normalized = value.strip().lower()
    return (
        normalized.startswith(("$", "<", "{", "test", "fake", "example", "placeholder", "packaged"))
        or normalized in {"configured", "missing", "redacted", "secret", "your-key-here"}
        or "<redacted>" in normalized
    )


def _secret_finding(line: str, relative_path: str = "") -> str:
    if PRIVATE_KEY_PATTERN.search(line):
        return "private-key material"
    if DIRECT_OPENAI_KEY_PATTERN.search(line):
        return "OpenAI-style credential"
    for assignment in LITERAL_PROVIDER_KEY_PATTERN.finditer(line):
        name, value = assignment.group(1).upper(), assignment.group(2)
        if (relative_path, name, value) in AUDITED_CREDENTIAL_FIXTURES:
            continue
        if not _placeholder_secret(value):
            return f"literal {name} credential"
    return ""


def validate_tracked_secret_content(repo_root: Path, paths: Iterable[str]) -> list[dict[str, str]]:
    """Scan relevant tracked source/config files without ever echoing a matched value."""
    errors: list[dict[str, str]] = []
    for relative_path in paths:
        normalized = relative_path.replace("\\", "/")
        path = repo_root / normalized
        if path.suffix.lower() not in SECRET_SCAN_SUFFIXES:
            continue
        try:
            if path.stat().st_size > 2 * 1024 * 1024:
                continue
            content = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line_number, line in enumerate(content.splitlines(), 1):
            finding = _secret_finding(line, normalized)
            if finding:
                errors.append(
                    _issue("security.secret_content", f"{finding} must not be tracked: {normalized}:{line_number}")
                )
    return errors


def validate_git_secret_content(repo_root: Path) -> list[dict[str, str]]:
    """Use Git's index scan to avoid opening every tracked Orca source file on Windows."""
    pathspecs = [f"*{suffix}" for suffix in sorted(SECRET_SCAN_SUFFIXES)]
    result = _run_git(
        repo_root,
        [
            "grep",
            "-n",
            "-I",
            "-E",
            "-e",
            "HY3D_API|OPENAI_PRO_API|OPENAI_API_KEY|TRIPO_API_KEY|sk-|PRIVATE KEY",
            "--",
            *pathspecs,
        ],
    )
    if result.returncode == 1:
        return []
    if result.returncode != 0:
        return [_issue("security.scan_failed", "tracked secret content scan could not be completed")]

    errors: list[dict[str, str]] = []
    for record in result.stdout.splitlines():
        relative_path, separator, remainder = record.partition(":")
        line_text, line_separator, line = remainder.partition(":")
        if not separator or not line_separator or not line_text.isdigit():
            continue
        finding = _secret_finding(line, relative_path.replace("\\", "/"))
        if finding:
            errors.append(
                _issue(
                    "security.secret_content",
                    f"{finding} must not be tracked: {relative_path.replace(chr(92), '/')}:{line_text}",
                )
            )
    return errors


def validate_integration_receipts(
    document: dict[str, Any], repo_root: Path, head: str
) -> tuple[list[dict[str, str]], dict[str, Any]]:
    """Verify historical snapshot-port receipts without scanning the whole worktree."""
    errors: list[dict[str, str]] = []
    receipt_details: dict[str, Any] = {}
    for feature_name, receipt in document["integration_receipts"].items():
        source_sha = receipt["source_sha"]
        integration_commit = receipt["integration_commit"]
        feature_details: dict[str, Any] = {
            "source_sha": source_sha,
            "integration_commit": integration_commit,
            "verified_paths": [],
        }
        receipt_details[feature_name] = feature_details
        if source_sha != document["feature_sources"][feature_name]["sha"]:
            errors.append(
                _issue(
                    "git.receipt_source",
                    f"{feature_name} receipt source does not match the accepted feature source",
                )
            )
            continue
        if _run_git(repo_root, ["cat-file", "-e", f"{integration_commit}^{{commit}}"]).returncode != 0:
            errors.append(
                _issue(
                    "git.receipt_commit",
                    f"{feature_name} integration receipt commit is unavailable: {integration_commit}",
                )
            )
            continue
        if _run_git(repo_root, ["merge-base", "--is-ancestor", integration_commit, head]).returncode != 0:
            errors.append(
                _issue(
                    "git.receipt_ancestor",
                    f"{feature_name} integration receipt {integration_commit} is not an ancestor of HEAD {head}",
                )
            )
            continue
        for path, expected_object in receipt["verified_git_objects"].items():
            source_object = _git_object_at_path(repo_root, source_sha, path)
            integration_object = _git_object_at_path(repo_root, integration_commit, path)
            if source_object is None or integration_object is None:
                errors.append(
                    _issue(
                        "git.receipt_path",
                        f"{feature_name} receipt path is missing from its source or integration commit: {path}",
                    )
                )
                continue
            if source_object != expected_object or integration_object != expected_object:
                errors.append(
                    _issue(
                        "git.receipt_object",
                        f"{feature_name} receipt object mismatch for {path}",
                    )
                )
                continue
            feature_details["verified_paths"].append(path)
    return errors, receipt_details


def validate_git(document: dict[str, Any], repo_root: Path) -> tuple[list[dict[str, str]], dict[str, Any]]:
    """Validate that locked commits exist and remain tied to the intended histories."""
    errors: list[dict[str, str]] = []
    details: dict[str, Any] = {}
    head_result = _run_git(repo_root, ["rev-parse", "HEAD"])
    if head_result.returncode != 0:
        return [_issue("git.repository", "repository HEAD could not be resolved")], details
    head = head_result.stdout.strip()
    details["head"] = head

    tracked_result = _run_git(repo_root, ["ls-files"])
    if tracked_result.returncode != 0:
        errors.append(_issue("git.tracked_files", "tracked files could not be enumerated"))
    else:
        tracked_paths = tracked_result.stdout.splitlines()
        errors.extend(validate_tracked_paths(tracked_paths))
        errors.extend(validate_git_secret_content(repo_root))

    shas = {"upstream": document["upstream"]["sha"]}
    shas.update({name: source["sha"] for name, source in document["feature_sources"].items()})
    for name, sha in shas.items():
        if _run_git(repo_root, ["cat-file", "-e", f"{sha}^{{commit}}"]).returncode != 0:
            errors.append(_issue("git.missing_commit", f"locked {name} commit is not available: {sha}"))

    upstream_sha = document["upstream"]["sha"]
    if _run_git(repo_root, ["merge-base", "--is-ancestor", upstream_sha, head]).returncode != 0:
        errors.append(_issue("git.upstream_ancestor", f"locked upstream {upstream_sha} is not an ancestor of HEAD {head}"))

    # feature_sources records historical imports, not the current developer
    # branch tips. Fixed source objects and ancestor receipts below preserve
    # provenance even when the original branch refs are absent or have moved.
    receipt_errors, receipt_details = validate_integration_receipts(document, repo_root, head)
    errors.extend(receipt_errors)
    details["integration_receipts"] = receipt_details
    return errors, details


def _source_requirements(document: dict[str, Any]) -> tuple[tuple[str, str, str], ...]:
    runtime = document["runtime_contract"]
    version = re.escape(runtime["sidecar_version"])
    port = runtime["product_port"]
    protocol = runtime["protocol_version"]
    return (
        (
            "tools/ai/orca_ai_sidecar.py",
            rf'^PORT\s*=\s*int\(os\.environ\.get\("ORCASLICER_AI_SIDECAR_PORT",\s*"{port}"\)\)',
            "Sidecar default port",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            rf'^SIDECAR_VERSION\s*=\s*"{version}"',
            "Sidecar version",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            rf'"protocol_version"\s*:\s*{protocol}\b',
            "Sidecar protocol",
        ),
        (
            "src/slic3r/GUI/AIServiceManager.cpp",
            rf'DEFAULT_LOCAL_ENDPOINT\s*=\s*"http://127\.0\.0\.1:{port}"',
            "service-manager endpoint",
        ),
        (
            "src/slic3r/GUI/AIServiceManager.cpp",
            rf'EXPECTED_PROTOCOL_VERSION\s*=\s*{protocol}\b',
            "service-manager protocol",
        ),
        (
            "src/slic3r/GUI/AIServiceManager.cpp",
            rf'EXPECTED_SIDECAR_VERSION\s*=\s*"{version}"',
            "service-manager Sidecar version",
        ),
        (
            "src/slic3r/GUI/AISidecarClient.cpp",
            rf'return\s+"http://127\.0\.0\.1:{port}"',
            "Sidecar client endpoint",
        ),
        (
            "src/slic3r/GUI/AISidecarClient.cpp",
            r'request\.local_only\(\);',
            "native Sidecar local-only transport",
        ),
        (
            "src/slic3r/GUI/AISidecarClient.cpp",
            r'X-OrcaSlicer-Session-Proof',
            "native Sidecar session proof",
        ),
        (
            "src/slic3r/Utils/Http.cpp",
            r'p->follow_redirects\s*=\s*false;',
            "local HTTP redirect denial",
        ),
        (
            "src/slic3r/Utils/Http.cpp",
            r'CURLOPT_NOPROXY,\s*"\*"',
            "local HTTP proxy bypass",
        ),
        (
            "src/slic3r/Utils/Http.cpp",
            r'CURLOPT_PROXY,\s*""',
            "local HTTP explicit proxy disable",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            r'X-OrcaSlicer-Session-Proof',
            "Sidecar request proof validation",
        ),
        (
            "tools/ai/orca_ai_installed_bootstrap.py",
            r'os\.environ\["ORCASLICER_AI_REQUIRE_SESSION"\]\s*=\s*"1"',
            "installed Sidecar session requirement",
        ),
        (
            "tools/ai/orca_ai_installed_bootstrap.py",
            r'build_identity_invalid',
            "installed Sidecar build identity gate",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            r'def\s+_monitor_parent\s*\(',
            "installed Sidecar parent lifetime monitor",
        ),
        (
            "src/slic3r/GUI/AIServiceManager.cpp",
            r'process::environment\s+child_environment\s*;',
            "Sidecar child environment allowlist",
        ),
        (
            "src/slic3r/GUI/AIServiceManager.cpp",
            r'ORCASLICER_AI_PARENT_PID',
            "Sidecar parent identity propagation",
        ),
        (
            "CMakeLists.txt",
            r'ORCA_AI_RUNTIME_DEPENDENCIES_FILE',
            "bundled runtime dependency manifest",
        ),
        (
            "src/CMakeLists.txt",
            r'verify_bundled_runtime\.py',
            "installed runtime verification",
        ),
        (
            "CMakeLists.txt",
            r'Commercial AI packages require an exact 40-character source commit',
            "commercial package source identity fail-closed gate",
        ),
        (
            "src/slic3r/BuildInfo.cpp.in",
            r'return\s+"@ORCA_SOURCE_COMMIT@"',
            "isolated exact build identity",
        ),
        (
            ".github/workflows/build_all.yml",
            r"vars\.SELF_HOSTED\s*&&\s*github\.event_name\s*!=\s*'pull_request'",
            "pull-request self-hosted runner denial",
        ),
        (
            ".github/CODEOWNERS",
            r"^/docs/architecture/\s+\S+",
            "architecture documentation CODEOWNER",
        ),
        (
            ".github/CODEOWNERS",
            r"^/src/slic3r/AI/CMakeLists\.txt\s+\S+",
            "AI composition CODEOWNER",
        ),
        (
            ".github/CODEOWNERS",
            r"^/src/slic3r/AI/Contracts/\s+\S+",
            "neutral AI contract CODEOWNER",
        ),
        (
            ".github/CODEOWNERS",
            r"^/src/slic3r/GUI/AI/Model/ModelFinishing\.\*\s+\S+",
            "model finishing CODEOWNER",
        ),
        (
            document["ci_contract"]["guardrails_workflow"],
            r"^    name:\s*['\"]?AI integration checks['\"]?\s*$",
            "required AI integration check name",
        ),
        (
            document["ci_contract"]["candidate_workflow"],
            r"^    name:\s*['\"]?Team integration candidate['\"]?\s*$",
            "required team integration candidate check name",
        ),
        (
            document["ci_contract"]["guardrails_workflow"],
            r"^  merge_group:",
            "AI guardrails merge queue trigger",
        ),
        (
            document["ci_contract"]["candidate_workflow"],
            r"^  merge_group:",
            "team candidate merge queue trigger",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            r'/v1/orcaslicer/shutdown',
            "authenticated Sidecar graceful shutdown",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            r'_restore_jobs\(resume_jobs=False\)',
            "deferred remote-job recovery",
        ),
    )


FORBIDDEN_SOURCE_PATTERNS = (
    (
        "CMakeLists.txt",
        r'add_definitions\s*\([^)]*GIT_COMMIT_HASH',
        "source identity must not be a target-wide compiler definition",
    ),
    (
        "src/slic3r/GUI/AIServiceManager.cpp",
        r'boost::this_process::environment\s*\(',
        "Sidecar child must not inherit the complete desktop environment",
    ),
    (
        ".github/workflows/build_all.yml",
        r'secrets:\s*inherit',
        "general build workflow must not inherit repository secrets",
    ),
    (
        ".github/workflows/build_check_cache.yml",
        r'secrets:\s*inherit',
        "cache/build workflow must not forward repository secrets",
    ),
    (
        ".github/workflows/build_deps.yml",
        r'secrets:\s*inherit',
        "dependency/build workflow must not forward repository secrets",
    ),
    (
        ".github/workflows/build_orca.yml",
        r'ORCA_UPDATER_SIG_KEY',
        "general build job must not expose updater signing credentials",
    ),
)


def validate_source_constants(document: dict[str, Any], repo_root: Path) -> list[dict[str, str]]:
    errors: list[dict[str, str]] = []
    for relative_path, pattern, label in _source_requirements(document):
        path = repo_root / relative_path
        try:
            content = path.read_text(encoding="utf-8")
        except OSError as exc:
            errors.append(_issue("source.missing", f"cannot read {relative_path}: {exc}"))
            continue
        if re.search(pattern, content, flags=re.MULTILINE) is None:
            errors.append(_issue("source.constant", f"{label} does not match the lock in {relative_path}"))
    for relative_path, pattern, label in FORBIDDEN_SOURCE_PATTERNS:
        path = repo_root / relative_path
        try:
            content = path.read_text(encoding="utf-8")
        except OSError:
            continue
        if re.search(pattern, content, flags=re.MULTILINE) is not None:
            errors.append(_issue("source.forbidden", f"{label}: {relative_path}"))
    return errors


def validate_contract_layout(repo_root: Path) -> list[dict[str, str]]:
    """Validate neutral contract ownership and source-compatible forwarding headers."""
    errors: list[dict[str, str]] = []
    for relative_path in NEUTRAL_CONTRACT_HEADERS:
        if not (repo_root / relative_path).is_file():
            errors.append(_issue("contract.missing", f"neutral contract is missing: {relative_path}"))

    for relative_path, expected in LEGACY_CONTRACT_FORWARDERS.items():
        path = repo_root / relative_path
        try:
            content = path.read_text(encoding="utf-8")
        except OSError as exc:
            errors.append(_issue("contract.missing", f"cannot read legacy contract header {relative_path}: {exc}"))
            continue
        if content != expected:
            errors.append(
                _issue("contract.forwarder", f"legacy contract header must be an exact forwarder: {relative_path}")
            )

    for relative_path, required_includes in DIRECT_CONTRACT_CONSUMERS.items():
        path = repo_root / relative_path
        try:
            content = path.read_text(encoding="utf-8")
        except OSError as exc:
            errors.append(_issue("contract.consumer", f"cannot read direct contract consumer {relative_path}: {exc}"))
            continue
        for required_include in required_includes:
            if required_include not in content:
                errors.append(
                    _issue(
                        "contract.consumer",
                        f"direct neutral contract include is missing from {relative_path}: {required_include}",
                    )
                )
        for legacy_path in LEGACY_CONTRACT_FORWARDERS:
            legacy_include = f'#include "{legacy_path.removeprefix("src/")}"'
            if legacy_include in content:
                errors.append(
                    _issue(
                        "contract.consumer",
                        f"integration consumer must not use legacy contract include in {relative_path}: {legacy_include}",
                    )
                )
    return errors


def _cmake_call_body(content: str, command: str, target: str) -> str:
    matches = re.findall(
        rf"{re.escape(command)}\s*\(\s*{re.escape(target)}\b(.*?)\)",
        content,
        flags=re.DOTALL,
    )
    return "\n".join(matches)


def _cmake_set_body(content: str, variable: str) -> str | None:
    match = re.search(
        rf"set\s*\(\s*{re.escape(variable)}\b(.*?)\n\s*\)",
        content,
        flags=re.DOTALL,
    )
    return match.group(1) if match else None


def validate_build_info_generation(repo_root: Path) -> list[dict[str, str]]:
    """Keep the configure-time BuildInfo source safe for multi-config generators."""
    relative_path = "src/slic3r/CMakeLists.txt"
    path = repo_root / relative_path
    try:
        content = path.read_text(encoding="utf-8")
    except OSError as exc:
        return [_issue("build.missing", f"cannot read {relative_path}: {exc}")]

    errors: list[dict[str, str]] = []
    configure_pattern = re.compile(
        r"configure_file\s*\(\s*\$\{CMAKE_CURRENT_SOURCE_DIR\}/BuildInfo\.cpp\.in"
        r"\s+\$\{ORCA_BUILD_INFO_SOURCE\}\s+@ONLY\s*\)",
        flags=re.DOTALL,
    )
    if configure_pattern.search(content) is None:
        errors.append(_issue("build.identity_source", "BuildInfo.cpp must remain a configure-time source"))

    generated_pattern = re.compile(
        r'set_source_files_properties\s*\(\s*"?\$\{ORCA_BUILD_INFO_SOURCE\}"?\s+PROPERTIES'
        r"[^)]*\bGENERATED\s+(?:TRUE|ON|1)\b",
        flags=re.DOTALL | re.IGNORECASE,
    )
    if generated_pattern.search(content) is not None:
        errors.append(
            _issue(
                "build.multiconfig_generated_source",
                "configure_file output BuildInfo.cpp must not be marked GENERATED; "
                "Ninja Multi-Config creates invalid cross-config order dependencies",
            )
        )
    return errors


def validate_ai_build_boundaries(repo_root: Path) -> list[dict[str, str]]:
    """Validate explicit targets and exclusive SmartSlicing core source ownership."""
    errors: list[dict[str, str]] = []
    relative_path = "src/slic3r/AI/CMakeLists.txt"
    path = repo_root / relative_path
    try:
        content = path.read_text(encoding="utf-8")
    except OSError as exc:
        return [_issue("build.missing", f"cannot read {relative_path}: {exc}")]
    gui_relative_path = "src/slic3r/CMakeLists.txt"
    gui_path = repo_root / gui_relative_path
    try:
        gui_content = gui_path.read_text(encoding="utf-8")
    except OSError as exc:
        return [_issue("build.missing", f"cannot read {gui_relative_path}: {exc}")]

    target_patterns = (
        (r"add_library\s*\(\s*orcaslicer_ai_contracts\s+INTERFACE\s*\)", "AI contracts interface target"),
        (
            r"add_library\s*\(\s*OrcaSlicer::AIContracts\s+ALIAS\s+orcaslicer_ai_contracts\s*\)",
            "AI contracts namespaced alias",
        ),
        (
            r"add_library\s*\(\s*orcaslicer_ai_smart_slicing\s+STATIC\s+\$\{ORCA_AI_SMART_SLICING_SOURCES\}\s*\)",
            "SmartSlicing static target",
        ),
        (
            r"add_library\s*\(\s*OrcaSlicer::SmartSlicing\s+ALIAS\s+orcaslicer_ai_smart_slicing\s*\)",
            "SmartSlicing namespaced alias",
        ),
    )
    for pattern, label in target_patterns:
        if re.search(pattern, content, flags=re.DOTALL) is None:
            errors.append(_issue("build.target", f"{label} is missing from {relative_path}"))

    contract_includes = _cmake_call_body(content, "target_include_directories", "orcaslicer_ai_contracts")
    if "INTERFACE" not in contract_includes or "${CMAKE_CURRENT_SOURCE_DIR}/../.." not in contract_includes:
        errors.append(_issue("build.target", "AI contracts target must export the src include root"))
    contract_links = _cmake_call_body(content, "target_link_libraries", "orcaslicer_ai_contracts")
    if "INTERFACE" not in contract_links or "boost_headeronly" not in contract_links:
        errors.append(_issue("build.link", "AI contracts must propagate the Boost header dependency"))

    smart_links = _cmake_call_body(content, "target_link_libraries", "orcaslicer_ai_smart_slicing")
    if "PUBLIC" not in smart_links or "OrcaSlicer::AIContracts" not in smart_links:
        errors.append(_issue("build.link", "SmartSlicing must link the neutral contracts target publicly"))
    if re.search(r"libslic3r_gui|wxWidgets", smart_links):
        errors.append(_issue("build.link", "SmartSlicing core must not link GUI dependencies"))

    if re.search(r"add_subdirectory\s*\(\s*AI\s*\)", gui_content) is None:
        errors.append(_issue("build.link", f"{gui_relative_path} must compose the AI subdirectory"))

    gui_links = _cmake_call_body(gui_content, "target_link_libraries", "libslic3r_gui")
    for required_target in ("OrcaSlicer::AIContracts", "OrcaSlicer::SmartSlicing"):
        if required_target not in gui_links:
            errors.append(_issue("build.link", f"libslic3r_gui must link {required_target}"))

    smart_sources = _cmake_set_body(content, "ORCA_AI_SMART_SLICING_SOURCES")
    gui_sources = _cmake_set_body(gui_content, "SLIC3R_GUI_SOURCES")
    if smart_sources is None:
        errors.append(_issue("build.target", "ORCA_AI_SMART_SLICING_SOURCES is missing"))
    if gui_sources is None:
        errors.append(_issue("build.target", "SLIC3R_GUI_SOURCES is missing"))
    for source in SMART_SLICING_CORE_SOURCES:
        occurrence_count = content.count(source)
        if smart_sources is None or source not in smart_sources:
            errors.append(_issue("build.source_ownership", f"SmartSlicing target does not own {source}"))
        gui_source = f"AI/{source}"
        if gui_sources is not None and gui_source in gui_sources:
            errors.append(_issue("build.source_ownership", f"libslic3r_gui still owns {gui_source}"))
        if occurrence_count != 1:
            errors.append(
                _issue(
                    "build.source_ownership",
                    f"{source} must appear exactly once in {relative_path}; found {occurrence_count}",
                )
            )
    return errors


def validate_gui_feature_boundaries(repo_root: Path) -> list[dict[str, str]]:
    """Keep Orca shared GUI files as thin composition and event-forwarding roots."""
    errors: list[dict[str, str]] = []
    for relative_path in GUI_FEATURE_FILES:
        if not (repo_root / relative_path).is_file():
            errors.append(_issue("gui.feature_host", f"required GUI feature unit is missing: {relative_path}"))

    def read(relative_path: str) -> str:
        try:
            return (repo_root / relative_path).read_text(encoding="utf-8")
        except OSError as exc:
            errors.append(_issue("gui.missing", f"cannot read {relative_path}: {exc}"))
            return ""

    main_cpp = read("src/slic3r/GUI/MainFrame.cpp")
    main_hpp = read("src/slic3r/GUI/MainFrame.hpp")
    plater_cpp = read("src/slic3r/GUI/Plater.cpp")
    panel_cpp = read("src/slic3r/GUI/ModelGenerationPanel.cpp")
    gui_cmake = read("src/slic3r/CMakeLists.txt")

    for forbidden in (
        'ModelGenerationPanel.hpp',
        'AI/Orca/OrcaWorkspaceAdapter.hpp',
        'AIServiceManager.hpp',
        'AISidecarClient.hpp',
        'm_ai_service_manager',
        'm_ai_service_retry_timer',
        'm_ai_orca_workspace',
    ):
        if forbidden in main_cpp or forbidden in main_hpp:
            errors.append(_issue("gui.mainframe_boundary", f"MainFrame directly owns AI implementation detail: {forbidden}"))
    if 'AI/AIDesktopFeatureHost.hpp' not in main_cpp or 'AIDesktopFeatureHost' not in main_hpp:
        errors.append(_issue("gui.mainframe_boundary", "MainFrame must compose AIDesktopFeatureHost only"))

    for forbidden in (
        'AI/Orca/OrcaSmartSlicingAdapter.hpp',
        'AI/SmartSlicing/SmartSlicingPanel.hpp',
        'AI/SmartSlicing/SmartSlicingPresenter.hpp',
        'SmartSlicingCoordinator',
        'OrcaTrialSliceExecutor',
        'OrcaOfficialSliceGateway',
        'OrcaWorkflowRuntimeStore',
    ):
        if forbidden in plater_cpp:
            errors.append(_issue("gui.plater_boundary", f"Plater directly owns smart-slicing implementation detail: {forbidden}"))
    if 'AI/SmartSlicing/SmartSlicingFeatureHost.hpp' not in plater_cpp:
        errors.append(_issue("gui.plater_boundary", "Plater must delegate smart-slicing composition to SmartSlicingFeatureHost"))

    if "class ModelPreview3D final" in panel_cpp:
        errors.append(_issue("gui.model_panel_boundary", "ModelPreview3D must be extracted from ModelGenerationPanel.cpp"))
    for include in (
        'AI/ModelGeneration/ModelGenerationPresentation.hpp',
        'AI/ModelGeneration/ModelPreview3D.hpp',
    ):
        if include not in panel_cpp:
            errors.append(_issue("gui.model_panel_boundary", f"ModelGenerationPanel must use extracted unit: {include}"))

    for relative_path in GUI_FEATURE_FILES:
        cmake_source = relative_path.removeprefix("src/slic3r/")
        if cmake_source not in gui_cmake:
            errors.append(_issue("build.source_ownership", f"libslic3r_gui does not own {cmake_source}"))
    return errors


def validate_model_generation_import_boundary(repo_root: Path) -> list[dict[str, str]]:
    """Ensure model generation imports into Prepare without owning slicing policy or execution."""
    errors: list[dict[str, str]] = []
    forbidden_by_path = {
        "src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp": (
            "auto_slice_after_import",
            "slice_after_import",
        ),
        "src/slic3r/GUI/ModelGenerationPanel.cpp": (
            "auto_slice_after_import",
            "slice_after_import",
            "导入后自动切片",
        ),
        "src/slic3r/GUI/ModelGenerationPanel.hpp": ("m_auto_slice_after_import",),
        "src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp": (
            "auto_slice_after_import",
            "slice_after_import",
            "independent_support_layer_height",
            "enable_prime_tower",
        ),
        "src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp": (
            "std::function<void(bool)>",
        ),
        "src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.hpp": (
            "std::function<void(bool slice)>",
        ),
        "src/slic3r/GUI/AI/AIDesktopFeatureHost.hpp": (
            "std::function<void(bool slice)>",
        ),
    }
    for relative_path, forbidden_tokens in forbidden_by_path.items():
        path = repo_root / relative_path
        if not path.is_file():
            continue
        content = path.read_text(encoding="utf-8", errors="replace")
        for token in forbidden_tokens:
            if token in content:
                errors.append(
                    _issue(
                        "model_generation.auto_slice",
                        f"{relative_path} still owns automatic slicing token: {token}",
                    )
                )

    main_path = repo_root / "src/slic3r/GUI/MainFrame.cpp"
    if main_path.is_file():
        main_cpp = main_path.read_text(encoding="utf-8", errors="replace")
        start = main_cpp.find("m_ai_feature_host = std::make_unique<AIDesktopFeatureHost>")
        end = main_cpp.find("m_tabpanel->AddPage(TAB_ID_GENERATE_3D", start)
        if start >= 0 and end > start:
            import_callback = main_cpp[start:end]
            for token in ("EVT_GLTOOLBAR_SLICE_PLATE", "TAB_ID_PREVIEW"):
                if token in import_callback:
                    errors.append(
                        _issue(
                            "model_generation.auto_slice",
                            f"MainFrame model-generation import callback still triggers slicing: {token}",
                        )
                    )
    return errors


def _cpp_files(path: Path) -> Iterable[Path]:
    if path.is_file():
        if path.suffix.lower() in {".cc", ".cpp", ".h", ".hpp"}:
            yield path
        return
    if path.is_dir():
        for candidate in sorted(path.rglob("*")):
            if candidate.is_file() and candidate.suffix.lower() in {".cc", ".cpp", ".h", ".hpp"}:
                yield candidate


def _dependency_issue(repo_root: Path, path: Path, line_number: int, rule: str) -> dict[str, str]:
    relative_path = path.relative_to(repo_root).as_posix()
    return _issue("boundary.dependency", f"{relative_path}:{line_number} violates {rule}")


def validate_dependency_boundaries(repo_root: Path) -> list[dict[str, str]]:
    """Scan the dependency directions that keep the feature lanes independent."""
    errors: list[dict[str, str]] = []
    include_pattern = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')

    model_locations = (
        repo_root / "src/slic3r/AI/ModelGeneration",
        repo_root / "src/slic3r/GUI/AIModelGenerationClient.cpp",
        repo_root / "src/slic3r/GUI/AIModelGenerationClient.hpp",
        repo_root / "src/slic3r/GUI/ModelGenerationPanel.cpp",
        repo_root / "src/slic3r/GUI/ModelGenerationPanel.hpp",
    )
    for location in model_locations:
        for path in _cpp_files(location):
            content = path.read_text(encoding="utf-8", errors="replace")
            for line_number, line in enumerate(content.splitlines(), 1):
                normalized = line.replace("\\", "/")
                if "SmartSlicing/" in normalized or "SmartSlicing::" in normalized:
                    errors.append(
                        _dependency_issue(
                            repo_root,
                            path,
                            line_number,
                            "model-generation isolation",
                        )
                    )

    contracts_location = repo_root / "src/slic3r/AI/Contracts"
    forbidden_contract_include = re.compile(
        r"(^|/)(ModelGeneration|SmartSlicing|GUI)(/|$)|(^|/)wx[^/]*(/|$)|tripo|openai|tools[/\\]ai",
        re.IGNORECASE,
    )
    for path in _cpp_files(contracts_location):
        content = path.read_text(encoding="utf-8", errors="replace")
        for line_number, line in enumerate(content.splitlines(), 1):
            match = include_pattern.match(line)
            if match and forbidden_contract_include.search(match.group(1).replace("\\", "/")):
                errors.append(_dependency_issue(repo_root, path, line_number, "neutral contract isolation"))

    forbidden_domain_include = re.compile(r"(^|/)(wx|gui)(/|$)|plater|provider|tripo|openai", re.IGNORECASE)
    for layer in ("Domain", "Application"):
        location = repo_root / "src/slic3r/AI/SmartSlicing" / layer
        for path in _cpp_files(location):
            content = path.read_text(encoding="utf-8", errors="replace")
            for line_number, line in enumerate(content.splitlines(), 1):
                match = include_pattern.match(line)
                if match and forbidden_domain_include.search(match.group(1).replace("\\", "/")):
                    errors.append(
                        _dependency_issue(
                            repo_root,
                            path,
                            line_number,
                            "SmartSlicing Domain/Application include isolation",
                        )
                    )

    smart_gui = repo_root / "src/slic3r/GUI/AI/SmartSlicing"
    forbidden_gui_reference = re.compile(
        r"ModelGenerationPanel|model_provider_gateway|openai_preprocessor|tripo_client|tools[/\\]ai",
        re.IGNORECASE,
    )
    for path in _cpp_files(smart_gui):
        content = path.read_text(encoding="utf-8", errors="replace")
        for line_number, line in enumerate(content.splitlines(), 1):
            if forbidden_gui_reference.search(line):
                errors.append(
                    _dependency_issue(repo_root, path, line_number, "SmartSlicing GUI provider/model-panel isolation")
                )
    return errors


def validate(lock_path: Path, repo_root: Path, skip_git: bool = False) -> dict[str, Any]:
    report: dict[str, Any] = {
        "ok": False,
        "schema": SCHEMA,
        "lock_file": str(lock_path),
        "git_checks_skipped": skip_git,
        "errors": [],
    }
    try:
        document = json.loads(lock_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        report["errors"] = [_issue("lock.read", f"cannot read lock file: {exc}")]
        return report

    errors = validate_document(document)
    if not errors:
        errors.extend(validate_source_constants(document, repo_root))
        errors.extend(validate_contract_layout(repo_root))
        errors.extend(validate_build_info_generation(repo_root))
        errors.extend(validate_ai_build_boundaries(repo_root))
        errors.extend(validate_gui_feature_boundaries(repo_root))
        errors.extend(validate_model_generation_import_boundary(repo_root))
        errors.extend(validate_dependency_boundaries(repo_root))
        errors.extend(validate_architecture_budgets(document, repo_root))
        if not skip_git:
            git_errors, git_details = validate_git(document, repo_root)
            errors.extend(git_errors)
            report["git"] = git_details
    report["errors"] = errors
    report["ok"] = not errors
    return report


def _default_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def main(argv: Sequence[str] | None = None) -> int:
    repo_root = _default_repo_root()
    parser = argparse.ArgumentParser(description="Validate OrcaSlicer AI integration pins and boundaries.")
    parser.add_argument(
        "--lock-file",
        type=Path,
        default=repo_root / "docs" / "architecture" / "ai-integration-lock.json",
        help="Path to the integration lock JSON.",
    )
    parser.add_argument("--skip-git", action="store_true", help="Skip commit/object/ancestry checks.")
    parser.add_argument("--json", action="store_true", help="Emit a machine-readable JSON report.")
    args = parser.parse_args(argv)

    report = validate(args.lock_file.resolve(), repo_root, skip_git=args.skip_git)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, sort_keys=True))
    elif report["ok"]:
        print("AI integration guardrails: PASS")
        if args.skip_git:
            print("Git checks: skipped by request")
        elif report.get("git", {}).get("head"):
            print(f"HEAD: {report['git']['head']}")
    else:
        print("AI integration guardrails: FAIL", file=sys.stderr)
        for error in report["errors"]:
            print(f"- [{error['code']}] {error['message']}", file=sys.stderr)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
