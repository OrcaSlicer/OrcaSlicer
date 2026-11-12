#!/usr/bin/env bash
# Sourceable helper: `retry <cmd...>` runs a command, retrying up to 3 times
# with a 5-minute wait between attempts. Useful for flaky commands such as
# `hdiutil create` intermittently failing with "Resource busy".
retry() {
  local attempt=1 max_attempts=3 delay=300
  until "$@"; do
    if [ "$attempt" -ge "$max_attempts" ]; then
      echo "::error::Command failed after $attempt attempts: $*"
      return 1
    fi
    echo "Attempt $attempt failed: $*. Retrying in $((delay / 60)) minutes..."
    sleep "$delay"
    attempt=$((attempt + 1))
  done
}
