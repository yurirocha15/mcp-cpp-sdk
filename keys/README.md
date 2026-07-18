# Release verification material

Published releases use `release-signing-key.asc` as their canonical OpenPGP
verification key. The file contains only the release primary public key and
the active public tag/artifact signing subkeys.

Private keys, passphrases, revocation certificates, recovery material, tokens,
and exported secret-key packets must never be committed here or anywhere else
in the repository.

The release workflow verifies its configured public fingerprints against this
file before it makes any publisher credential available.
