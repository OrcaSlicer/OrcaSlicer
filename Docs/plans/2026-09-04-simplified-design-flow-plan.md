# Simplified Design Flow Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Deliver three style families, one-click recommended-color image generation, unlimited color, and best-design-image Tripo input/display.

**Architecture:** Reuse the native wxWidgets presentation/client contracts and Python job state machine. Preserve legacy style IDs and history while simplifying visible choices. Separate color intent from the image used for geometry, and retain single-request billing boundaries.

**Tech Stack:** C++17, wxWidgets, Python, Pillow, unittest, Catch2, MSVC/CMake.

---

## Tasks
1. Read native panel/presentation/client, sidecar job parsing/persistence/generation, history, and build verification paths; snapshot existing overlapping edits.
2. Add failing tests for style family mapping, unconstrained palette behavior, recommended palette auto-continuation without a second click, and generated-image routing independent of exact colors.
3. Implement compact native settings/primary action and compatible restore behavior in ModelGenerationPanel and ModelGenerationPresentation. Keep translated labels consistent with existing source conventions.
4. Implement any additive client/sidecar state required for the one-click journey, preserving the legacy manual-confirmation API and no implicit paid retries.
5. Select/show best AI design images, remove strict palette/intermediate choices from normal UI, retain multiview, and verify Tripo payload/reference paths. Do not remove useful final-model color intent.
6. Run targeted red/green and complete tools/ai suite; native presentation tests and Release compile/link; inspect the desktop UI where available without triggering paid generation.
7. Stage verified runtime changes with backup, do not interrupt an active app/job without permission; report restart and any environment limitations. No unrelated commit, release or push.

## Execution
Implement in the current task on its existing branch. The superpowers-prefixed skills are unavailable; use the available Code workflow and scoped file plan. No subagents unless the user asks for parallel agents.
