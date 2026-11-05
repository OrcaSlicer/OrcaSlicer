# Contributing to OrcaSlicer

Thanks for wanting to help! OrcaSlicer is built by volunteers.

Review is the slowest part of getting a PR merged. A small group of reviewers works through hundreds of open PRs in their spare time. If a PR has what they need, it is reviewed sooner and is more likely to be merged. [How to Contribute](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute) on the wiki has the details for each kind of PR.

For questions about using OrcaSlicer, please ask on [Discord](https://discord.gg/P4VE9UY9gJ) or in [Discussions](https://github.com/OrcaSlicer/OrcaSlicer/discussions), not in an issue or PR.

Opening an issue? See [Opening an Issue](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#opening-an-issue).

## The short version

- Search open PRs and issues before you start. Someone may already be working on it.
- Keep each PR to one topic. Put changes that belong together in one PR, and unrelated changes in separate PRs. Focused PRs are reviewed and merged much faster.
- Don't open many PRs at once. If you already have several open, wait until some are merged.
- Explain why the change is needed, and show how you tested it. Add an automated test when the code can be tested.
- AI tools are welcome. You are still responsible for the change, so test it and make sure you understand it.
- Keep your PR up to date and reply to review comments.

## Write a description reviewers can use

Use the [PR template](.github/pull_request_template.md), and keep its "How to Download Pull Requests Artifacts" link as the last line. Testers use that link. In the template, answer these questions:

- **What does this change, and why is it needed?** If it fixes an issue, write `Fixes #1234` in the description, so GitHub closes the issue when the PR is merged. If it fixes only part of an issue, mention `#1234` without `Fixes`, so the issue stays open.
- **Are there related PRs or issues?** If another PR touches the same code or bug, explain how yours is different.
- **How did you test it?** Give the OS, the printer profile and the steps. Attach a project file if the result depends on a model or settings. Name any automated tests you added. The best test fails without your change and passes with it. See [tests/AGENTS.md](tests/AGENTS.md) for where a test goes.
- **What could you not test?** For example another OS, or a printer you don't own.

Use short, plain sentences, because many reviewers read English as a second language. Your own English is fine, and so is a translation tool.

## What each kind of PR needs

| Kind of PR | Label | Reviewers need to see |
|---|---|---|
| [Bug fix](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#bug-fixes) | `bug-fix`, `crash` | The cause, before and after, a test if possible |
| [Security fix](SECURITY.md) | `SECURITY` | A private report to the maintainers first |
| [New feature or setting](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#new-features-and-settings) | `enhancement`, `QoL` | Why users need it, that it is off by default, real results |
| [Performance](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#performance) | `optimization` | Measurements, and proof the output did not change |
| [Refactor](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#refactors) | `optimization` | That behavior and output did not change |
| [GUI change](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#gui-changes) | `UI/UX` | Screenshots before and after, on each platform you tested |
| [Printer profile](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#printer-profiles) | `profile` | The validator passing, the version bump, real print results |
| [Translation](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#translations) | `Localization` | Glossary terms used, placeholders unchanged |
| [Build, CI or dependencies](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#build-ci-and-dependencies) | `build`, `dependencies` | Which platforms you built on |
| [Tests](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#tests) | `test` | That the test fails when the behavior breaks |
| [Documentation](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#documentation) | `documentation` | That it matches the current code |

## Rules that are easy to miss

- **Don't reformat existing code or make unrelated edits.** `.clang-format` is for new files. In existing files, match the code around your change.
- **Include what you use.** Because of the precompiled header, a local build can succeed with a missing include. Run `scripts/run_clang_tidy.sh` (or `.ps1`) before you push.
- **New behavior is off by default.** When the new option is off, existing output must not change.
- **Keep old projects and profiles working.** Never change what an existing setting means. Add a new key, and convert old values in `handle_legacy`.
- **Write each UI message as one translatable string** with placeholders, never as joined fragments. See "Writing translatable strings in the source" in [AGENTS.md](AGENTS.md).
- **Only edit `msgstr` in translations.** Keep every placeholder and `\n` exactly as in the `msgid`.
- **Bump the vendor version when you change profiles.** Change `version` in `resources/profiles/<Vendor>.json` for every vendor you change, and run `scripts/check_profile.sh` (or `.bat`).

The code style and the full list of what reviewers check are in [AGENTS.md](AGENTS.md).

## Using AI tools

AI tools are welcome here, and many contributors and maintainers use them. Reviewers can only spend time on PRs that a person has already tested.

- **You are responsible for the change.** Before you ask for review, build the change, run it, and confirm that it works. Be ready to explain each change and why it is needed.
- **Keep it short and true.** AI tools tend to write long descriptions, and sometimes describe tests that never ran. Remove generated code that isn't needed, too. A short description of what you actually did helps more.
- **Read what you post.** Please don't paste a review comment into an AI tool and post its answer without checking it.
- **Don't let an agent open PRs or post comments on its own.** Read everything before it is posted.
- **Use the repository's skills.** `.claude/skills/` (also reachable as `.agents/skills/`) has instructions for AI tools on parts of OrcaSlicer. The top of each skill's `SKILL.md` describes when to use it. Some tools load them automatically. With others, point the tool to the skill that matches your change.

### If you are an AI agent

If you are an AI agent working for a contributor, please follow these steps:

1. Show the contributor this guide before you open a PR or post a comment.
2. Search open PRs for the same change before you start.
3. If the contributor already has several open PRs, wait until some are merged before you open another.
4. Before you change code, check `.claude/skills/` for a skill that matches the change, and follow it.
5. If the code can be tested, add an automated test, and check that it fails without the change and passes with it.
6. Ask the contributor to test the change by hand. For a bug fix, they should build the code without the change and reproduce the bug. Then they should build the branch and check that the bug is gone.
7. Write the PR description in the [PR template](.github/pull_request_template.md). A body passed with `gh pr create --body` replaces the template, so start from the template's text.
8. If you couldn't build or run something, say so in the PR.

## After you open the PR

- **Add labels.** A bot will comment with instructions. Reply with the labels from the table above, for example `/bot add-label bug-fix, UI/UX`.
- **Leave "Allow edits by maintainers" turned on**, so a reviewer can push small fixes directly.
- **Mark the PR ready for review when it's finished.** Draft PRs are not merged.
- **Push new commits to the same PR.** Please don't open a new PR for the same change.
- **Don't force-push once a reviewer has commented.** Add new commits instead, so reviewers can see what changed since they last looked. To fix merge conflicts, merge `main` into your branch. PRs are usually squash-merged, so extra commits are fine.
- **Reply to every review comment**, including automated ones. You don't have to agree, but please say why.

Reviewers are volunteers, and a first response can take a while. If you already have several PRs open, please wait until some are merged before you open more. Review comments on one PR often apply to the others too. While you wait, there are [other ways to help](https://www.orcaslicer.com/wiki/developer_reference/how_to_contribute#other-ways-to-help).

## If your PR is closed

There are more PRs than reviewers can handle, so we may close a PR that doesn't meet this guide. For example, it may have no explanation of how it was tested, duplicate another PR, or look generated and untested. When we close a PR, we'll say why in a comment.

You're welcome to open a new PR once the change meets this guide. Please link the old PR in the description, so reviewers can see the earlier discussion. Thank you for the time you put into it.

## Security issues

Please don't report security problems in a public issue. See [SECURITY.md](SECURITY.md).

## Thank you

Thanks again for helping. We're glad you're here.
