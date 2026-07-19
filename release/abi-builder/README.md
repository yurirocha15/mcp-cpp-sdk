# Canonical ABI builder

This image is the reviewed build environment for the
`ubuntu-noble-amd64-gcc13-libstdcxx-abigail2.4` ABI corpus. Its Ubuntu base
manifest and package archive snapshot are immutable inputs. The release job
must still use the resulting registry image by its `sha256` digest, never by a
tag.

The minimal Ubuntu base does not contain CA certificates. The Dockerfile's
first APT operation temporarily disables TLS peer checking only to install the
CA bundle from the same snapshot. Ubuntu's signed `InRelease` metadata and
package digests remain mandatory; unauthenticated packages and trusted-source
overrides are never enabled. The override is removed and a second snapshot
update with normal TLS verification must succeed before tools are installed.

Build and publish it once from this directory with a private staging tag:

```console
docker buildx build --platform linux/amd64 --provenance=false --sbom=false \
  --tag "$ABI_BUILDER_STAGING_TAG" --push .
docker buildx imagetools inspect "$ABI_BUILDER_STAGING_TAG"
```

Copy the complete `registry/path@sha256:<64 lowercase hex characters>` result
to the protected repository variable `ABI_BUILDER_IMAGE`. Review any change to
the base digest, snapshot, or package list as an ABI-toolchain migration. The
corpus identity also records and hashes the exact installed package contents
and the relevant compiler, runtime, dependency, CMake, and libabigail files.
