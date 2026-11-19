# Building and publishing releases

The [Build and release workflow](../.github/workflows/release.yml) runs **only on pushes to `main`**. It does not publish from pull requests, tag pushes, other branches or a manual-dispatch event. Merging a PR into `main` produces a push and starts the workflow. Rerun a failed push run from the Actions UI after resolving its cause.

## Version and tag

The authoritative application version is `SP_VERSION` in [`src/host.h`](../src/host.h). It currently uses a regular `MAJOR.MINOR.PATCH` version. Releases consistently use `vMAJOR.MINOR.PATCH` (for example, `v1.1.0`); the workflow never derives a version from a branch name or commit message.

When changing the version, keep these existing representations synchronized:

- `src/host.h`: the `SP_VERSION` wide string.
- `src/app.rc`: numeric `FILEVERSION` and `PRODUCTVERSION` (`MAJOR,MINOR,PATCH,0`) and string `FileVersion` / `ProductVersion` (`MAJOR.MINOR.PATCH`).
- `src/app.manifest`: assembly identity version (`MAJOR.MINOR.PATCH.0`).
- Version-sensitive test expectations: `tests/test_host.c` currently asserts the CLI version text; update it with a version bump so the existing native test gate checks the new release rather than the old one.

Use decimal components without leading zeroes, each in `0..65535` for Windows VERSIONINFO. Prerelease/build suffixes are not supported by this regular-release workflow. The prepared executable's `--version`, embedded File/Product versions, x64 GUI PE type and stripped COFF symbol table must agree with the source before packaging. A stale executable or mismatched version source fails the build job rather than publishing a misleading tag.

## Build and artifacts

The read-only Windows build job checks out the **exact pushed commit**, downloads **Zig 0.17.1** from ziglang.org, verifies its pinned official SHA-256 and checks the compiler version. It uses the existing documented build path, in order:

```bat
build.cmd test
build.cmd release
build\test_host.exe build/release/cloakspace.exe
```

The native suites run sequentially; the host/lifecycle suite also runs against the exact optimized release executable. The release helper additionally validates all six shipped sample/profile combinations with that executable. A failure of any build, test, version, packaging or artifact-upload step prevents the dependent publisher from running. Native suites require a working desktop; a hosted-runner desktop/input failure is a failed build, not a reason to bypass the tests. Physical shortcuts, monitor/DPI combinations and real optional Terminal/Podman integrations still need manual verification.

Each release has two uploaded assets:

- `cloakspace-vMAJOR.MINOR.PATCH-windows-x64.zip`: the regular **unsigned** `cloakspace.exe`, `scratchpad.conf`, `README.md`, `LICENSE`, `docs/` and `examples/`, with no build caches, test executables, object/resource files or debug symbols. Only Markdown under `docs/` and `.conf` / `.c` / `.h` files under `examples/` are allowed; hidden, linked or unexpected inputs fail packaging rather than silently shipping. Extract it into its own directory and read the README before running.
- `SHA256SUMS`: the ZIP's SHA-256 in standard `hash  filename` format. In PowerShell, compare it with `Get-FileHash .\cloakspace-vMAJOR.MINOR.PATCH-windows-x64.zip -Algorithm SHA256`; in Git Bash, use `sha256sum --check SHA256SUMS`.

GitHub also provides its **Source code (zip)** and **Source code (tar.gz)** downloads for the same tag, including the corresponding C source, tests and both build scripts. The tag is pinned to the commit that produced the binary, not the current head of `main` at publication time. ZIP entry timestamps are normalized so the same inputs can be packaged consistently.

## Publication safety and reruns

Only the dependent Ubuntu publication job has `contents: write`; the compiler/test job has `contents: read`, and all unspecified workflow permissions are disabled. Checkout never persists credentials. All actions are pinned to full commit SHAs. Publishing uses GitHub's automatically provided `GITHUB_TOKEN` (`github.token`), not a personal token or a new repository secret. Repository/organization policy must allow Actions and release creation; a denied API request fails visibly.

Same-version publication jobs are serialized without cancelling an in-progress publication. After a successful build, the publisher checks the exact asset inventory and checksum, then checks existing tags and every page of authenticated releases (including drafts) by exact tag. Ambiguous same-tag releases fail before any write; GitHub's published-only release-by-tag endpoint is not used to discover drafts. It:

1. Creates a missing tag only at the exact built commit. It never moves/deletes an existing tag. A tag for another commit without a completed release is an error: bump the version or resolve the conflict explicitly.
2. Creates or resumes a **draft** release for that commit. A foreign-commit draft, unexpected draft asset, prerelease or corrupt published release fails rather than being overwritten.
3. Uploads the ZIP and checksum to the draft, reads the complete asset list back, and verifies their names, uploaded states, sizes and GitHub SHA-256 digests before publishing. An upload failure leaves only a tag/draft, not a partial public release. A rerun can finish the same commit's draft; only incomplete/mismatched assets in that draft may be replaced.
4. Publishes a regular release, then reads the release, tag and assets back again before reporting success. GitHub's `legacy` latest-release selection avoids blindly making a slower, older-version build the latest release.

An existing completed release with the same version is validated and **left unchanged**, including on later pushes with no version bump. Those pushes still build/test but do not replace released bytes or move the original tag. Change the version to publish new program changes. A broken published release is not silently repaired by automation; investigate it manually. Do not delete/recreate public releases or retag commits as a retry strategy.

## Package publishing

**No package publishing target exists.** This is a native C/Win32 executable repository with CMD/Bash build scripts, not an npm, Python, NuGet, Cargo or other registry package. It has no package manifest, installer/container publishing target, registry configuration or established package-publishing process. The workflow therefore publishes GitHub Release assets only and requests neither `packages: write` nor package-registry credentials. Optional Terminal/Podman runtime integrations are not packages produced by this build.

## Checking workflow changes locally

Application users still need only the prerequisites in the README. Contributors changing the release automation can additionally use PowerShell 7.2+ and Node.js 24+ (both supplied by the selected GitHub runners):

```powershell
node --test .github/tests/publish-release.test.cjs
.\build.cmd test
.\build.cmd release
.\build\test_host.exe build/release/cloakspace.exe
.\.github\tests\prepare-release.tests.ps1
.\.github\scripts\prepare-release.ps1
```

The Node tests use an explicitly in-memory GitHub double: **no network publication or credentials**. The preparation tests use temporary fixtures under ignored `build/`, exercise the real release executable, verify byte-for-byte archive contents/reproducibility and reject invalid versions/metadata/configurations. The helper writes the final ZIP/checksum under ignored `build/release-assets/`; it requires an empty output directory and never cleans another distribution for you. For another local package, choose a new empty directory with `-OutputDirectory build/release-assets-another`.

Validate workflow syntax with [actionlint](https://github.com/rhysd/actionlint), for example `actionlint .github/workflows/release.yml`. Local tests and lint do not claim a hosted workflow has run: the first push to `main` containing this workflow must still be observed in Actions and its release assets checked on GitHub.
