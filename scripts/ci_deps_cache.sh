#!/usr/bin/env bash
# Lets a pull request's macOS deps build, or its clang-tidy job, wait for a
# build that saves the deps cache it needs, from a Linux job. `check` runs after
# a cache miss and names a queued or running push build of the base branch with
# the same deps/ and .github/ to wait for, unless the pull request changes either.
# `own` names the pull request's own build instead, for clang-tidy when the pull
# request changes deps/. `wait` waits until that build saves KEY under CACHE_REF,
# or its deps job fails. actions/cache then confirms the entry can be restored.
#
# Environment: GH_TOKEN, REPO, WORKFLOW_REF (or WORKFLOW, the build workflow's
# file name) and BASE_REF, plus HEAD_SHA for `own`, and KEY_INPUTS, WAIT_FOR,
# WAIT_SINCE, WAIT_MINUTES, KEY, CACHE_REF (default the base branch) and ARCH
# (or JOB, the matrix job's name) for `wait`.
# Writes key-inputs, wait-for and wait-since to $GITHUB_OUTPUT.
set -uo pipefail

workflow=${WORKFLOW_REF:-}
workflow=${workflow%%@*}
workflow=${WORKFLOW:-${workflow##*/}}

# Must match the job names in build_all.yml, build_check_cache.yml and build_deps.yml.
job=${JOB:-build_macos_arch (${ARCH:-})}
check_job_name="$job / Check Cache"
deps_job_name="$job / Build Deps / Build Deps"

out() { echo "$1" >> "${GITHUB_OUTPUT:-/dev/stdout}"; }

# The cache key depends on deps/ and on the workflows under .github/.
local_key_inputs() { echo "$(git rev-parse "$1:.github") $(git rev-parse "$1:deps")"; }
remote_key_inputs() {
  gh api "repos/$REPO/git/trees/$1" < /dev/null \
    --jq '[.tree[] | select(.path == ".github" or .path == "deps") | {key: .path, value: .sha}] | from_entries | "\(.[".github"]) \(.deps)"'
}

# Prints the id of a push build of the base branch that has not finished, is not
# one of the ids in $1, and has KEY_INPUTS.
matching_base_build() {
  local status runs id sha
  runs=$(for status in in_progress queued pending waiting requested; do
    gh api -X GET "repos/$REPO/actions/workflows/$workflow/runs" \
      -f branch="$BASE_REF" -f event=push -f status="$status" -f per_page=10 < /dev/null \
      | jq -r '.workflow_runs[] | "\(.id) \(.head_sha)"'
  done)
  while read -r id sha; do
    [ -n "$id" ] || continue
    [[ " ${1:-} " == *" $id "* ]] && continue
    if [ "$(remote_key_inputs "$sha")" = "$KEY_INPUTS" ]; then
      echo "$id"
      return
    fi
  done <<< "$runs"
}

check() {
  KEY_INPUTS=$(local_key_inputs HEAD)
  out "key-inputs=$KEY_INPUTS"
  if [ "$KEY_INPUTS" != "$(local_key_inputs HEAD^1)" ]; then
    echo "This pull request changes deps/ or .github/, so it builds its own deps."
    return
  fi
  local id
  id=$(matching_base_build)
  if [ -n "$id" ]; then
    echo "Build $id of $BASE_REF has the same deps/ and .github/. Waiting for its cache."
    out "wait-for=$id"
    out "wait-since=$(date +%s)"
  else
    echo "No build of $BASE_REF is building these deps."
  fi
}

own() {
  out "key-inputs=$(local_key_inputs HEAD)"
  local id
  # The build can be created a moment after this job.
  for _ in 1 2 3 4 5; do
    id=$(gh api -X GET "repos/$REPO/actions/workflows/$workflow/runs" \
      -f head_sha="$HEAD_SHA" -f event=pull_request -f per_page=1 --jq '.workflow_runs[0].id // empty' < /dev/null)
    [ -n "$id" ] && break
    sleep 30
  done
  if [ -n "$id" ]; then
    echo "Waiting for the deps cache of build $id of this pull request."
    out "wait-for=$id"
    out "wait-since=$(date +%s)"
  else
    echo "No build of this pull request is building its deps."
  fi
}

key_saved() {
  gh cache list -R "$REPO" --ref "${CACHE_REF:-refs/heads/$BASE_REF}" --key "$KEY" --json key < /dev/null \
    | jq -e --arg key "$KEY" 'any(.[]; .key == $key)' > /dev/null
}

wait_for() {
  # Counts the limit from the check, including time queued behind other waiting
  # pull requests.
  local id=$WAIT_FOR seen=$WAIT_FOR deadline=$((${WAIT_SINCE:-$(date +%s)} + WAIT_MINUTES * 60)) jobs job check warned=
  while [ "$(date +%s)" -lt "$deadline" ]; do
    if key_saved; then
      echo "Found $KEY."
      return
    fi
    jobs=$(gh api -X GET "repos/$REPO/actions/runs/$id/jobs" -f filter=latest -f per_page=100 < /dev/null)
    job=$(jq -r --arg name "$deps_job_name" 'first(.jobs[] | select(.name == $name) | "\(.status) \(.conclusion)") // ""' <<< "$jobs")
    case "$job" in
      "completed success" | "completed skipped")
        # The new entry can take a moment to be listed, and a skipped job means
        # the build found the cache, possibly under the default branch.
        for _ in 1 2 3; do
          key_saved && { echo "Found $KEY."; return; }
          sleep 20
        done
        echo "Build $id finished its deps without saving $KEY."
        return ;;
      "completed cancelled") ;;
      completed*)
        echo "The deps job of build $id did not succeed."
        return ;;
      # The deps job exists only once the build's cache check has succeeded.
      "")
        case "$(gh api "repos/$REPO/actions/runs/$id" --jq '"\(.status) \(.conclusion)"' < /dev/null)" in
          "completed cancelled") ;;
          completed*)
            check=$(jq -r --arg name "$check_job_name" 'first(.jobs[] | select(.name == $name) | .conclusion) // ""' <<< "$jobs")
            if [ -z "$warned" ] && { [ -z "$check" ] || [ "$check" = success ]; }; then
              echo "::warning title=Deps wait::Build $id has no job named \"$deps_job_name\". If the build jobs were renamed, update scripts/ci_deps_cache.sh. This does not affect this pull request."
              warned=1
            fi ;;
          *)
            sleep 120
            continue ;;
        esac ;;
      *)
        sleep 120
        continue ;;
    esac
    # Follows another unfinished build with the same key inputs, such as the
    # push that replaced a cancelled one.
    id=$(matching_base_build "$seen")
    [ -n "$id" ] || break
    seen="$seen $id"
    echo "Following build $id of $BASE_REF."
  done
  echo "No build saved $KEY."
}

case "${1:-}" in
  check) check ;;
  own) own ;;
  wait) wait_for ;;
  *) echo "usage: $0 check|own|wait" >&2; exit 2 ;;
esac
