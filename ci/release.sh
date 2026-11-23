#!/bin/bash
# release.sh VERSION SHA IPK_DIR SRC_DIR: attaches a finished CI build to the pre-release v<VERSION> (build.yml, job
# Release). A pre-release is never "latest", so neither Homebrew Channel's main repository (it reads
# releases/latest/download/<id>.manifest.json) nor webosbrew/apps.json offer it as an update: publish.yml does that.
#   notes:    the "## New in <VERSION>" section of SRC_DIR/README.md; none = no notes
#   manifest: the app's entry in SRC_DIR/webosbrew/apps.json with this version, IPK URL, sha256 and size
# A pre-release of the same version is replaced (a rebuild); a published release is never touched.
# NOTES_ONLY=1 prints the notes and the manifest without touching GitHub (for testing).
set -euo pipefail
VER=$1 SHA=$2 IPK_DIR=$3 SRC=$4
REPO=${GITHUB_REPOSITORY:-sarahahzub/WebOS-Chromium}
TAG=v$VER
cfg() { sed -n "s/^$1=\"\{0,1\}\([^\"#]*\)\"\{0,1\}.*/\1/p" "$SRC/source/config.env" | sed 's/ *$//'; }
ID=$(cfg APP_ID) TITLE=$(cfg APP_TITLE)
[ -n "$ID" ] || { echo "APP_ID missing in source/config.env"; exit 1; }
IPK=$IPK_DIR/${ID}_${VER}_arm.ipk
[ -f "$IPK" ] || { echo "no $IPK"; ls -la "$IPK_DIR"; exit 1; }
OUT=$(mktemp -d)

python3 - "$SRC/README.md" "$VER" "${TITLE:-Chromium Browser}" > "$OUT/notes.md" <<'EOF'
import re, sys
readme, ver, title = sys.argv[1], sys.argv[2], sys.argv[3]
lines, body, inside = open(readme, encoding="utf-8").read().splitlines(), [], False
for l in lines:
    if re.match(r"^## ", l):
        if inside:
            break
        inside = re.match(r"^## New in v?%s\s*$" % re.escape(ver), l) is not None
        continue
    if inside:
        body.append(l)
text = "\n".join(body).strip()
if text:
    print("## %s %s\n\n%s" % (title, ver, text))
EOF

python3 - "$SRC/webosbrew/apps.json" "$ID" "$TITLE" "$VER" "$IPK" "$REPO" "$TAG" > "$OUT/$ID.manifest.json" <<'EOF'
import hashlib, json, os, sys
apps, pid, title, ver, ipk, repo, tag = sys.argv[1:8]
base = {}
try:
    base = next(p["manifest"] for p in json.load(open(apps))["packages"] if p["id"] == pid)
except (OSError, StopIteration, KeyError, ValueError):
    pass
m = dict(base) or {"id": pid, "type": "native", "title": title, "rootRequired": False,
                   "sourceUrl": "https://github.com/" + repo}
m.update(id=pid, version=ver,
         ipkUrl="https://github.com/%s/releases/download/%s/%s" % (repo, tag, os.path.basename(ipk)),
         ipkHash={"sha256": hashlib.sha256(open(ipk, "rb").read()).hexdigest()},
         ipkSize=os.path.getsize(ipk))
print(json.dumps(m, indent=2))
EOF

if [ "${NOTES_ONLY:-0}" = 1 ]; then
	echo "== notes"; cat "$OUT/notes.md"; echo "== manifest"; cat "$OUT/$ID.manifest.json"; exit 0
fi

pre=$(gh api "repos/$REPO/releases/tags/$TAG" --jq '.prerelease' 2>/dev/null || echo none)
case "$pre" in
false) echo "::error::$TAG is already a published release: not changed"; exit 1 ;;
true)  echo "replacing the pre-release $TAG (rebuild)"; gh release delete "$TAG" -R "$REPO" --cleanup-tag -y ;;
esac
[ -s "$OUT/notes.md" ] && notes=(--notes-file "$OUT/notes.md") || { echo "no '## New in $VER' in README.md: no notes"; notes=(--notes ""); }
gh release create "$TAG" -R "$REPO" --target "$SHA" --title "${TITLE:-Chromium Browser} $VER" "${notes[@]}" \
	--prerelease --latest=false "$IPK" "$OUT/$ID.manifest.json"
url=$(gh release view "$TAG" -R "$REPO" --json url --jq .url)
echo "pre-release: $url"
[ -n "${GITHUB_STEP_SUMMARY:-}" ] && {
	echo "## Pre-release"
	echo
	echo "[$TAG]($url) with the IPK and the manifest. Not offered as an update yet: after testing, run **Actions → Publish update** with version $VER."
} >> "$GITHUB_STEP_SUMMARY"
exit 0
