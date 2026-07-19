#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
architecture="${1:?usage: validate_aur_packages.sh <architecture> <version> <release-assets>}"
version="${2:?usage: validate_aur_packages.sh <architecture> <version> <release-assets>}"
assets="${3:?usage: validate_aur_packages.sh <architecture> <version> <release-assets>}"
: "${RUNNER_TEMP:?RUNNER_TEMP is required}"

case "${architecture}" in
  x86_64) ;;
  *)
    echo 'validate-aur: unsupported architecture' >&2
    exit 2
    ;;
esac

check="${root}/aur-check"
raw_packages="$(mktemp "${RUNNER_TEMP}/aur-package-list.XXXXXX")"
validated_packages="$(mktemp "${RUNNER_TEMP}/aur-packages-validated.XXXXXX")"
shared_readelf="$(mktemp "${RUNNER_TEMP}/aur-shared-readelf.XXXXXX")"
static_readelf="$(mktemp "${RUNNER_TEMP}/aur-static-readelf.XXXXXX")"
export GNUPGHOME
GNUPGHOME="$(mktemp -d "${RUNNER_TEMP}/aur-validation-gnupg.XXXXXX")"

cleanup() {
  rm -rf -- "${GNUPGHOME}" "${raw_packages}" "${validated_packages}" \
    "${shared_readelf}" "${static_readelf}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM

python3 -I -S "${root}/scripts/release/collect_build_identity.py" \
  --kind aur --target "${architecture}" --output "${root}/aur-build-identity.json"
rm -rf -- "${check}"
mkdir -- "${check}"
install -m 644 "${assets}/aur-PKGBUILD" "${check}/PKGBUILD"
install -m 644 "${assets}/aur-SRCINFO" "${check}/.SRCINFO"
install -m 644 "${assets}/mcp-cpp-sdk-${version}.tar.gz" "${check}/"
install -m 644 "${assets}/mcp-cpp-sdk-${version}.tar.gz.asc" "${check}/"
install -m 644 "${assets}/release-signing-key.asc" "${check}/"

pushd "${check}" >/dev/null
if ! gpg --batch --quiet --import release-signing-key.asc >/dev/null 2>&1; then
  echo 'validate-aur: release verification key import failed' >&2
  exit 2
fi
makepkg --printsrcinfo >generated.SRCINFO
cmp --silent .SRCINFO generated.SRCINFO
makepkg --verifysource --noconfirm
makepkg --cleanbuild --noconfirm
makepkg --packagelist >"${raw_packages}"
popd >/dev/null

python3 -I -S "${root}/scripts/run_release_tool.py" release.aur_validation packages \
  --directory "${check}" --package-list "${raw_packages}" \
  --output "${validated_packages}" --version "${version}" \
  --architecture "${architecture}"
mapfile -t packages <"${validated_packages}"
test "${#packages[@]}" -eq 2
sudo /usr/local/libexec/mcp-install-aur-packages "${packages[@]}"
pacman -Q mcp-cpp-sdk mcp-cpp-sdk-static >/dev/null

python3 -I -S "${root}/scripts/run_release_tool.py" release.aur_validation consumer \
  --directory "${check}/consumer" --version "${version}"
cmake -S "${check}/consumer" -B "${check}/consumer-build" -G Ninja
cmake --build "${check}/consumer-build"
LC_ALL=C readelf --dynamic --wide "${check}/consumer-build/shared-consumer" >"${shared_readelf}"
LC_ALL=C readelf --dynamic --wide "${check}/consumer-build/static-consumer" >"${static_readelf}"
python3 -I -S "${root}/scripts/run_release_tool.py" release.aur_validation elf \
  --shared-readelf "${shared_readelf}" --static-readelf "${static_readelf}" \
  --version "${version}"
"${check}/consumer-build/shared-consumer"
"${check}/consumer-build/static-consumer"
