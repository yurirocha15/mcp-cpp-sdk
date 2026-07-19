#!/usr/bin/env bash

aur_ssh_cleanup() {
  if [[ "${AUR_SSH_AGENT_STARTED:-}" == true ]]; then
    ssh-agent -k >/dev/null 2>&1 || true
  fi
  rm -f "${AUR_SSH_ASKPASS_FILE:-}" "${AUR_SSH_PRIVATE_KEY_FILE:-}" \
    "${AUR_SSH_KNOWN_HOSTS_FILE:-}"
  unset AUR_SSH_AGENT_STARTED AUR_SSH_ASKPASS_FILE AUR_SSH_PRIVATE_KEY_FILE \
    AUR_SSH_KNOWN_HOSTS_FILE SSH_AUTH_SOCK SSH_AGENT_PID GIT_SSH_COMMAND
}

aur_ssh_setup() {
  local scope="${1:?AUR SSH scope is required}"
  : "${AUR_SSH_PRIVATE_KEY_B64:?}" "${AUR_SSH_KEY_PASSPHRASE:?}" "${AUR_KNOWN_HOSTS:?}"
  : "${RUNNER_TEMP:?}"

  umask 077
  AUR_SSH_ASKPASS_FILE="${RUNNER_TEMP}/${scope}-askpass"
  AUR_SSH_PRIVATE_KEY_FILE="${RUNNER_TEMP}/${scope}-key"
  AUR_SSH_KNOWN_HOSTS_FILE="${RUNNER_TEMP}/${scope}-known-hosts"
  export AUR_SSH_ASKPASS_FILE AUR_SSH_PRIVATE_KEY_FILE AUR_SSH_KNOWN_HOSTS_FILE

  python3 -I -S scripts/run_release_tool.py release.aur_ssh \
    --output "${AUR_SSH_KNOWN_HOSTS_FILE}"
  unset AUR_KNOWN_HOSTS
  printf '%s' "${AUR_SSH_PRIVATE_KEY_B64}" | base64 --decode >"${AUR_SSH_PRIVATE_KEY_FILE}"
  unset AUR_SSH_PRIVATE_KEY_B64
  chmod 600 "${AUR_SSH_PRIVATE_KEY_FILE}"
  printf '%s\n' '#!/bin/sh' 'printf "%s\\n" "${AUR_SSH_KEY_PASSPHRASE}"' \
    >"${AUR_SSH_ASKPASS_FILE}"
  chmod 700 "${AUR_SSH_ASKPASS_FILE}"
  export SSH_ASKPASS="${AUR_SSH_ASKPASS_FILE}" SSH_ASKPASS_REQUIRE=force DISPLAY=release
  unset SSH_AUTH_SOCK SSH_AGENT_PID
  eval "$(ssh-agent -s)" >/dev/null
  AUR_SSH_AGENT_STARTED=true
  export AUR_SSH_AGENT_STARTED
  ssh-add "${AUR_SSH_PRIVATE_KEY_FILE}" </dev/null >/dev/null 2>&1
  rm -f "${AUR_SSH_PRIVATE_KEY_FILE}"
  unset AUR_SSH_KEY_PASSPHRASE SSH_ASKPASS SSH_ASKPASS_REQUIRE DISPLAY
  rm -f "${AUR_SSH_ASKPASS_FILE}"

  GIT_SSH_COMMAND="ssh -F /dev/null -o BatchMode=yes -o ConnectTimeout=30 -o HostKeyAlgorithms=ssh-ed25519 -o PasswordAuthentication=no -o KbdInteractiveAuthentication=no -o StrictHostKeyChecking=yes -o UserKnownHostsFile=${AUR_SSH_KNOWN_HOSTS_FILE}"
  export GIT_SSH_COMMAND
}
