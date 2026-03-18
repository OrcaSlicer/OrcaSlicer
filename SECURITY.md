# Security Policy

OrcaSlicer follows responsible disclosure for security vulnerabilities.

## Reporting a Vulnerability

Please report security issues privately by email:

- **Contact:** `softfeverever@gmail.com`
- **Subject:** include `SECURITY`

Include as much of the following as possible:

- Affected version(s), OS/platform, and configuration.
- Reproduction steps.
- Potential impact and threat model.
- Proof-of-concept details, logs, screenshots, or sample files where relevant.

If helpful, use the OWASP disclosure checklist: <https://cheatsheetseries.owasp.org/cheatsheets/Vulnerability_Disclosure_Cheat_Sheet.html>.

## Response Expectations

- Initial acknowledgment target: **within 7 calendar days**.
- Follow-up target after acknowledgment: **within 48 hours** with next steps when triage has enough detail.

Complex reports may require additional back-and-forth; maintainers may ask for clarifications or validation artifacts.

## Coordination and Fixes

When a report is confirmed, maintainers will:

1. Validate and scope affected versions.
2. Audit for related variants.
3. Prepare and ship patches for maintained releases.
4. Coordinate disclosure timing when needed.

## Third-Party Components

For vulnerabilities originating in vendored or external dependencies, please also report upstream to the relevant project maintainers.

## User Safety Recommendations

- Download binaries only from official OrcaSlicer release channels.
- Avoid untrusted plugins, profiles, and scripts.
- Keep OrcaSlicer and printer firmware updated.
