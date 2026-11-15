# patches/

Our changes to webOS-ports/chromium151. `container/apply-patches.py` applies them in name order to a clean checkout
of the pinned commit (`C151_COMMIT` in `config.env`) before every build. Only `*.patch` files are applied.
`INDEX.md` lists them with the reason from each file's header.

How they're kept:

- One change per file, named `NNNN-short-name.patch`. Missing numbers are patches we merged into others or dropped.
- Paths start with `src/`, relative to the repo root.
- Each patch starts with a few `# why:` lines before the diff.
- An old patch isn't changed for something unrelated; that gets a new patch.

To apply them by hand:

```sh
git clone https://github.com/webOS-ports/chromium151 c151 && cd c151
git checkout 0baacb2b1a4b885826fed5493262f759c6a5b469
for p in /path/to/patches/*.patch; do git apply "$p" || break; done
```
