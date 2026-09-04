#!/usr/bin/env bash
set -euo pipefail

# -c rather than 'git config --local': the local form writes into .git/config and stays there, so a
# run on a workstation leaves every later commit in that clone attributed to GitHub Action. This
# script did exactly that here and 39 commits carry the wrong author because of it.
commit_as=(-c "user.email=action@github.com" -c "user.name=GitHub Action")

tmpdir="$(mktemp -d)"
cp -R build/docs/html/. "$tmpdir/"

# a first deploy has no gh-pages to branch from, so start an orphan rather than force-pushing
if git fetch origin gh-pages 2> /dev/null; then
	git branch --no-track gh-pages origin/gh-pages 2> /dev/null || true
	git switch -f gh-pages
else
	git switch --orphan gh-pages
fi

find . -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +

cp -R "$tmpdir"/. .
rm -rf "$tmpdir"
git add -A

if git diff --cached --quiet; then
	echo "No documentation changes to deploy."
	exit 0
fi

git "${commit_as[@]}" commit -m "Update Doxygen ($1)"
# the branch was built on origin/gh-pages, so this fast-forwards and never needs -f
git push origin gh-pages
