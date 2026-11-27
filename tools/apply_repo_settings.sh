#!/bin/sh
# Applies this repository's GitHub settings: the "settings as code" record of how the repository is protected.
# Run it as a repository admin with the GitHub CLI (gh auth login). Safe to run again: every step sets a value.
#
#   sh tools/apply_repo_settings.sh            # repository settings (needs the repo scope)
#   sh tools/apply_repo_settings.sh --org      # also the organisation's runner group (needs admin:org:
#                                              #   gh auth refresh -h github.com -s admin:org)
#
# What it sets, and why (docs/RELEASING.md has the release side):
#   - name stick-bridge (from zigbee-bridge; GitHub redirects the old name, which must never be reused)
#   - squash merges only, branches deleted after merging, linear history
#   - ruleset "main": every change through a pull request with all CI checks green and up to date with main,
#     conversations resolved, no force-push, no deletion, and no bypass for anyone (admins included). There is
#     one maintainer and GitHub doesn't let an author approve their own pull request, so no approval count is
#     required: the maintainer's merge is the approval.
#   - rulesets for v* tags: only admins create them; nobody moves or deletes them
#   - immutable releases: a published release's files and tag can't be changed
#   - environment "release": the maintainer approves each publish; only v* tags may deploy to it
#   - Actions: GitHub-owned actions only, pinned to full commit SHAs; the default token read-only and unable to
#     approve pull requests; every outside contributor's workflow run waits for approval
#   - secret scanning with push protection, Dependabot alerts and security updates, private vulnerability
#     reporting (CodeQL default setup is already on)
#   - secret PRIVATE_PATTERNS from ~/.config/stick-bridge/private-patterns.txt (the privacy check's patterns)
set -eu
OWNER=home-controls
NEW=stick-bridge
OLD=zigbee-bridge
org=false
[ "${1:-}" = --org ] && org=true

# gh inside a checkout on another drive can trip over git's ownership check
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*'

if gh api "repos/$OWNER/$NEW" --jq .name 2>/dev/null | grep -qx "$NEW"; then
  R=$OWNER/$NEW
else
  echo "== rename $OWNER/$OLD to $NEW"
  gh api -X PATCH "repos/$OWNER/$OLD" -f name="$NEW" --jq .full_name
  R=$OWNER/$NEW
fi
ME=$(gh api user --jq .id)
LOGIN=$(gh api user --jq .login)

echo "== repository options"
gh api -X PATCH "repos/$R" --silent \
  -f description="ESP32-P4 PoE bridge firmware: up to four USB radio sticks (Zigbee, Thread, Z-Wave) on the network as TCP ports, with a dashboard and stick flashing" \
  -F has_wiki=false -F has_projects=false -F has_issues=true \
  -F allow_squash_merge=true -F allow_merge_commit=false -F allow_rebase_merge=false \
  -f squash_merge_commit_title=PR_TITLE -f squash_merge_commit_message=COMMIT_MESSAGES \
  -F delete_branch_on_merge=true -F allow_update_branch=true -F allow_auto_merge=false
gh api -X PUT "repos/$R/topics" --silent \
  -f 'names[]=esp-idf' -f 'names[]=esp32' -f 'names[]=esp32-p4' -f 'names[]=ethernet' -f 'names[]=poe' \
  -f 'names[]=zigbee' -f 'names[]=zigbee2mqtt' -f 'names[]=zha' -f 'names[]=thread' -f 'names[]=z-wave' \
  -f 'names[]=home-assistant'

echo "== security features"
gh api -X PATCH "repos/$R" --silent --input - <<'EOF'
{"security_and_analysis": {
  "secret_scanning": {"status": "enabled"},
  "secret_scanning_push_protection": {"status": "enabled"},
  "secret_scanning_non_provider_patterns": {"status": "enabled"},
  "dependabot_security_updates": {"status": "enabled"}}}
EOF
gh api -X PUT "repos/$R/vulnerability-alerts" --silent
gh api -X PUT "repos/$R/private-vulnerability-reporting" --silent
gh api -X PUT "repos/$R/immutable-releases" --silent || echo "   (immutable releases not available: skipped)"

echo "== Actions"
gh api -X PUT "repos/$R/actions/permissions" --silent --input - <<'EOF'
{"enabled": true, "allowed_actions": "selected", "sha_pinning_required": true}
EOF
gh api -X PUT "repos/$R/actions/permissions/selected-actions" --silent --input - <<'EOF'
{"github_owned_allowed": true, "verified_allowed": false, "patterns_allowed": []}
EOF
gh api -X PUT "repos/$R/actions/permissions/workflow" --silent --input - <<'EOF'
{"default_workflow_permissions": "read", "can_approve_pull_request_reviews": false}
EOF
gh api -X PUT "repos/$R/actions/permissions/fork-pr-contributor-approval" --silent --input - <<'EOF'
{"approval_policy": "all_external_contributors"}
EOF

echo "== secret PRIVATE_PATTERNS"
PAT=${PRIVATE_PATTERNS_FILE:-$HOME/.config/stick-bridge/private-patterns.txt}
if [ -s "$PAT" ]; then gh secret set PRIVATE_PATTERNS -R "$R" < "$PAT"; else echo "   $PAT missing: CI's privacy check will fail until it is set"; fi

echo "== environment release (maintainer approves, v* tags only)"
gh api -X PUT "repos/$R/environments/release" --silent --input - <<EOF
{"wait_timer": 0, "prevent_self_review": false,
 "reviewers": [{"type": "User", "id": $ME}],
 "deployment_branch_policy": {"protected_branches": false, "custom_branch_policies": true}}
EOF
if ! gh api "repos/$R/environments/release/deployment-branch-policies" --jq '.branch_policies[].name' | grep -qx 'v*'; then
  gh api -X POST "repos/$R/environments/release/deployment-branch-policies" --silent -f name='v*' -f type=tag
fi

# a ruleset by name: update it if it exists, else create it
ruleset() {
  name=$1; body=$2
  id=$(gh api "repos/$R/rulesets" --jq ".[] | select(.name == \"$name\") | .id")
  if [ -n "$id" ]; then echo "$body" | gh api -X PUT "repos/$R/rulesets/$id" --silent --input -
  else echo "$body" | gh api -X POST "repos/$R/rulesets" --silent --input -; fi
  echo "   ruleset $name"
}

echo "== rulesets"
ruleset main '{
  "name": "main", "target": "branch", "enforcement": "active", "bypass_actors": [],
  "conditions": {"ref_name": {"include": ["~DEFAULT_BRANCH"], "exclude": []}},
  "rules": [
    {"type": "deletion"},
    {"type": "non_fast_forward"},
    {"type": "required_linear_history"},
    {"type": "pull_request", "parameters": {
      "required_approving_review_count": 0, "dismiss_stale_reviews_on_push": true,
      "require_code_owner_review": false, "require_last_push_approval": false,
      "required_review_thread_resolution": true, "allowed_merge_methods": ["squash"]}},
    {"type": "required_status_checks", "parameters": {
      "strict_required_status_checks_policy": true, "do_not_enforce_on_create": false,
      "required_status_checks": [
        {"context": "Host tests (ASan + UBSan)", "integration_id": 15368},
        {"context": "Dashboard page", "integration_id": 15368},
        {"context": "Version, workflows, privacy", "integration_id": 15368},
        {"context": "Firmware", "integration_id": 15368}]}}]}'
ruleset release-tags-immutable '{
  "name": "release-tags-immutable", "target": "tag", "enforcement": "active", "bypass_actors": [],
  "conditions": {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}},
  "rules": [{"type": "update"}, {"type": "deletion"}]}'
ruleset release-tags-admins-create '{
  "name": "release-tags-admins-create", "target": "tag", "enforcement": "active",
  "bypass_actors": [{"actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "always"}],
  "conditions": {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}},
  "rules": [{"type": "creation"}]}'

if $org; then
  echo "== organisation: runner group 'default' limited to the repositories that use it"
  # The TrueNAS runners sit in the default group, which otherwise serves every repository in the org,
  # public ones included. Only the listed repositories may use them; they still never run fork code
  # (see .github/workflows/ci.yml).
  gid=$(gh api "orgs/$OWNER/actions/runner-groups" --jq '.runner_groups[] | select(.default) | .id')
  ids=$(for r in "$NEW" control4-z2m; do gh api "repos/$OWNER/$r" --jq .id; done | paste -sd, -)
  gh api -X PATCH "orgs/$OWNER/actions/runner-groups/$gid" --silent --input - <<EOF
{"visibility": "selected", "allows_public_repositories": true}
EOF
  gh api -X PUT "orgs/$OWNER/actions/runner-groups/$gid/repositories" --silent --input - <<EOF
{"selected_repository_ids": [$ids]}
EOF
  gh api -X PATCH "orgs/$OWNER" --silent -F two_factor_requirement_enabled=true \
    || echo "   2FA requirement not set (GitHub only allows it when every member already has 2FA)"
fi

echo "== done: $R ($LOGIN approves releases). Check: https://github.com/$R/settings/rules"
