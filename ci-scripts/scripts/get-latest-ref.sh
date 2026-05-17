#!/bin/bash
# SPDX-License-Identifier: MIT

set -e

git fetch 2> /dev/null

# Find latest integration_* branch by commit date
latest_integration=$(git for-each-ref --format='%(refname:short) %(committerdate:unix)' refs/remotes/origin/integration_* | sort -k2 -nr | head -n1 | cut -d' ' -f1)

# Get latest commit date of develop
develop_commit_time=$(git log origin/develop -1 --format=%ct)

# Get latest commit date of latest integration branch
integration_commit_time=$(git log "$latest_integration" -1 --format=%ct)

# Choose latest branch
if [ "$develop_commit_time" -gt "$integration_commit_time" ]; then
    latest_branch="origin/develop"
else
    latest_branch="$latest_integration"
fi

# Remove 'origin/' prefix
branch_name=${latest_branch#origin/}

# Get commit ID of the selected branch
commit_id=$(git rev-parse "$latest_branch")

echo "$branch_name"
echo "$commit_id"
