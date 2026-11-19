# Homebrew Channel repository

`apps.json` is a [Homebrew Channel](https://github.com/webosbrew/webos-homebrew-channel) repository with two apps:
**Chromium Browser** (`io.github.sarahahzub.chromium`, 1.0.0 and newer) and **Chromium (old)**
(`org.nativechromium.browser`, whose last release 0.0.67 points users to Chromium Browser and hands their bookmarks
and history over). Add it on the TV in Homebrew Channel under **Settings → Add repository**:

```
https://raw.githubusercontent.com/sarahahzub/WebOS-Chromium/main/webosbrew/apps.json
```

`io.github.sarahahzub.chromium.manifest.json` is the Chromium Browser manifest on its own. Attach it to each GitHub
release, so the listing in the official repository can point at
`releases/latest/download/io.github.sarahahzub.chromium.manifest.json`. `org.nativechromium.browser.manifest.json`
describes the old app's last release (0.0.67) and doesn't change any more.

For every release, update `version`, `ipkUrl`, `ipkSize` and `ipkHash.sha256` in the Chromium Browser manifest and
its copy in `apps.json`. Homebrew Channel checks the hash, so they must describe the exact IPK attached to the release:

```
stat -c %s io.github.sarahahzub.chromium_<version>_arm.ipk
sha256sum io.github.sarahahzub.chromium_<version>_arm.ipk
```
