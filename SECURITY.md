# Security policy

## Reporting a vulnerability

Use **Report a vulnerability** in this repository's GitHub **Security** tab.
Do not open a public issue for suspected vulnerabilities, leaked credentials,
release-signing problems, or package-supply-chain incidents.

Include affected versions, reproduction details, impact, and any suggested
mitigation. Do not include third-party credentials, private keys, tokens, or
personal data in the report.

## Release integrity

Install a package route only after the exact version appears at its public
destination. The GitHub, Cloudsmith, and AUR publishers read the published
identity back before recording `PUBLISHED`; ConanCenter, Homebrew, and
Chocolatey remain pending while provider review or moderation is outstanding.
Release source archives, checksums, manifests, and SBOMs are published with
verification material. A registry upload, pending ledger entry, or open
packaging pull request is not by itself a supported release.

If release integrity is in doubt, stop installing the affected version and
report it privately through GitHub Security Advisories.
