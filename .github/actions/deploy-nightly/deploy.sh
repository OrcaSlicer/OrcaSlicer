#!/usr/bin/env bash
# Replaces one asset of a GitHub release, uploading under a temporary name
# first so a failed upload leaves the old asset in place.
#
# Environment: GH_TOKEN, ASSET_PATH, ASSET_NAME, CONTENT_TYPE, RELEASE_TAG and
# the GITHUB_* variables Actions sets.

# The $names inside the single-quoted jq programs are jq variables.
# shellcheck disable=SC2016
set -euo pipefail

repo_api="$GITHUB_API_URL/repos/$GITHUB_REPOSITORY"

if [ ! -f "$ASSET_PATH" ]; then
  echo "::error::$ASSET_PATH does not exist"
  exit 1
fi

# The speed limit turns a stalled transfer into an error that --retry handles.
curl_opts=(-sS --retry 3 --connect-timeout 30 --speed-limit 1024 --speed-time 60
  -H "Authorization: Bearer $GH_TOKEN")

# Prints the response body, or reports GitHub's error body and fails.
api() {
  local out
  if ! out=$(curl "${curl_opts[@]}" --fail-with-body "$@"); then
    # A workflow command ends at the first newline.
    echo "::error::GitHub API request failed: ${out//[$'\r\n']/ }" >&2
    return 1
  fi
  printf '%s\n' "$out"
}

http_status() {
  curl "${curl_opts[@]}" -o /dev/null -w '%{http_code}' "$@"
}

# jq prints CRLF on Windows.
jqr() {
  jq -r "$@" | tr -d '\r'
}

delete_asset() {
  local code
  code=$(http_status -X DELETE "$repo_api/releases/assets/$1")
  case $code in
    # A 404 means a concurrent run already deleted the asset.
    204 | 404) ;;
    *)
      echo "::error::Deleting release asset $1 failed with HTTP $code" >&2
      return 1
      ;;
  esac
}

list_assets() {
  local page=1 json
  while :; do
    # Callers run this inside $(...), where bash ignores set -e.
    json=$(api "$repo_api/releases/$release_id/assets?per_page=100&page=$page") || exit 1
    printf '%s\n' "$json"
    [ "$(jqr length <<<"$json")" -eq 100 ] || break
    page=$((page + 1))
  done | jq -s add
}

release=$(api "$repo_api/releases/tags/$RELEASE_TAG")
release_id=$(jqr .id <<<"$release")
upload_url=$(jqr .upload_url <<<"$release")
upload_url=${upload_url%%\{*}

# Unique per run attempt, so concurrent runs never upload to the same name.
tmp_name="$ASSET_NAME.partial-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT"

# Deletes leftover partials older than an hour, an age no upload in progress reaches.
stale_ids=$(list_assets | jqr --arg prefix "$ASSET_NAME.partial-" \
  '.[] | select((.name | startswith($prefix)) and (.created_at | fromdateiso8601) < now - 3600) | .id')
for id in $stale_ids; do
  delete_asset "$id"
done

name_query=$(jqr -n --arg name "$tmp_name" '$name | @uri')
# Uploads once, since a retry after an upload that succeeded fails on the taken name.
new_id=$(api --retry 0 -X POST -H "Content-Type: $CONTENT_TYPE" -T "$ASSET_PATH" \
  "$upload_url?name=$name_query" | jqr .id)
trap 'delete_asset "$new_id" || true' EXIT

rename_body=$(jq -n --arg name "$ASSET_NAME" '{name: $name}')
# Retries on HTTP 422, which a concurrent run causes by renaming its own upload
# between this delete and this rename.
for _ in 1 2 3; do
  old_ids=$(list_assets | jqr --arg name "$ASSET_NAME" '.[] | select(.name == $name) | .id')
  for id in $old_ids; do
    delete_asset "$id"
  done
  code=$(http_status -X PATCH -d "$rename_body" "$repo_api/releases/assets/$new_id")
  if [ "$code" = 200 ]; then
    trap - EXIT
    exit 0
  fi
  [ "$code" = 422 ] || break
done
echo "::error::Renaming $tmp_name to $ASSET_NAME failed with HTTP $code" >&2
exit 1
