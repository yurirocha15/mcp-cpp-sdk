#!/usr/bin/env bash
set -euo pipefail
set +x
umask 077

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
directory="${1:?usage: sign_release.sh <unsigned-directory>}"
: "${RUNNER_TEMP:?RUNNER_TEMP is required}"
: "${RELEASE_GPG_PRIVATE_KEY_B64:?RELEASE_GPG_PRIVATE_KEY_B64 is required}"
: "${RELEASE_GPG_PASSPHRASE:?RELEASE_GPG_PASSPHRASE is required}"
: "${ARTIFACT_SUBKEY_FINGERPRINT:?ARTIFACT_SUBKEY_FINGERPRINT is required}"

if [[ ! "${ARTIFACT_SUBKEY_FINGERPRINT}" =~ ^([0-9A-F]{40}|[0-9A-F]{64})$ ]]; then
  echo 'sign-release: artifact subkey fingerprint is malformed' >&2
  exit 2
fi
if [[ "${RELEASE_GPG_PASSPHRASE}" == *$'\n'* || "${RELEASE_GPG_PASSPHRASE}" == *$'\r'* ]]; then
  echo 'sign-release: GPG passphrase must be a single line' >&2
  exit 2
fi

plan="$(mktemp "${RUNNER_TEMP}/release-signing-plan.XXXXXX")"
key_file="$(mktemp "${RUNNER_TEMP}/release-signing-key.XXXXXX")"
export GNUPGHOME
GNUPGHOME="$(mktemp -d "${RUNNER_TEMP}/release-signing-gnupg.XXXXXX")"

cleanup() {
  unset RELEASE_GPG_PRIVATE_KEY_B64 RELEASE_GPG_PASSPHRASE
  rm -rf -- "${GNUPGHOME}" "${key_file}" "${plan}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM

python3 -I -S "${root}/scripts/run_release_tool.py" release.signing prepare \
  --directory "${directory}" --plan "${plan}"
printf '%s' "${RELEASE_GPG_PRIVATE_KEY_B64}" | base64 --decode >"${key_file}"
unset RELEASE_GPG_PRIVATE_KEY_B64
test -s "${key_file}"
if ! gpg --batch --quiet --import "${key_file}" >/dev/null 2>&1; then
  echo 'sign-release: private key import failed' >&2
  exit 2
fi
rm -f -- "${key_file}"
if ! gpg --batch --with-colons --list-secret-keys \
  "${ARTIFACT_SUBKEY_FINGERPRINT}!" >/dev/null 2>&1; then
  echo 'sign-release: configured artifact signing subkey is unavailable' >&2
  exit 2
fi

mapfile -t detached <"${plan}"
test "${#detached[@]}" -gt 0
for asset in "${detached[@]}"; do
  printf '%s\n' "${RELEASE_GPG_PASSPHRASE}" | gpg \
    --batch --yes --pinentry-mode loopback --passphrase-fd 0 \
    --local-user "${ARTIFACT_SUBKEY_FINGERPRINT}!" --armor --detach-sign \
    --output "${directory}/${asset}.asc" -- "${directory}/${asset}"
done

python3 -I -S "${root}/scripts/run_release_tool.py" release.signing finalize \
  --directory "${directory}" --plan "${plan}"
printf '%s\n' "${RELEASE_GPG_PASSPHRASE}" | gpg \
  --batch --yes --pinentry-mode loopback --passphrase-fd 0 \
  --local-user "${ARTIFACT_SUBKEY_FINGERPRINT}!" --armor --detach-sign \
  --output "${directory}/SHA256SUMS.asc" -- "${directory}/SHA256SUMS"
unset RELEASE_GPG_PASSPHRASE
python3 -I -S "${root}/scripts/run_release_tool.py" release.signing verify \
  --directory "${directory}" --plan "${plan}"
