# Pull-request version check

Every pull request targeting `main` must advance the application's **patch, minor,
or major** release, including documentation or CI-only pull requests. There are no
path filters, label exemptions, or branch-name conventions that bypass the check.

## Authoritative version

The check reads the existing `SP_VERSION` definition in [`src/host.h`](../src/host.h):

```c
#define SP_VERSION L"1.1.4"
```

The supported project format is a plain release `MAJOR.MINOR.PATCH`: three
non-negative decimal integers with no leading zeros (except `0` itself). Prefixes
such as `v`, prerelease suffixes, build metadata, computed macros, missing versions,
and duplicate definitions fail validation. No new version file is introduced.

Keep the corresponding Windows metadata in `src/app.rc` (`FILEVERSION`,
`PRODUCTVERSION`, `FileVersion`, and `ProductVersion`) and `src/app.manifest`
(`assemblyIdentity` version) in sync. Windows numeric metadata uses a fourth
component of `0`. The PR gate compares `SP_VERSION`; release validation is
responsible for verifying that the other metadata agrees.

## Comparison and examples

[The workflow](../.github/workflows/pr-version.yml) checks out the exact PR head SHA
and fetches all branch history using `actions/checkout` with `fetch-depth: 0`.
[The checker](../.github/scripts/check_pr_version.py) reads committed `src/host.h`
blobs from **`refs/remotes/origin/main`** and that **PR head SHA**, not the merge
base, synthetic PR merge result, working-tree version, commit message, or branch
name. Thus the base version comes from `main` as fetched when the job starts,
including main changes made since the PR branch diverged.

Comparison is numeric: `(PR major, PR minor, PR patch) > (main major, main minor,
main patch)`. A patch-only increase qualifies; unchanged or lower versions do not.

| `main` | PR | Result |
| --- | --- | --- |
| `1.1.0` | `1.1.1` | Pass: patch increase |
| `1.1.9` | `1.1.10` | Pass: numeric patch comparison |
| `1.1.0` | `1.2.0` | Pass: minor increase |
| `1.1.9` | `1.2.0` | Pass: patch may reset |
| `1.9.8` | `1.10.0` | Pass: numeric comparison |
| `1.99.9` | `2.0.0` | Pass: major increase |
| `1.1.0` | `1.1.0` | Fail: unchanged |
| `1.1.10` | `1.1.9` | Fail: lower patch |
| `1.2.0` | `1.1.999` | Fail: lower minor |
| `2.0.0` | `1.99.99` | Fail: lower major |

Missing or invalid versions on **either** side fail closed. The error names both
versions, identifies invalid input, and tells the author to update `SP_VERSION`
and matching Windows metadata. When main is valid it suggests a qualifying next
patch, minor, or major version. If main is invalid, repair its version before
rerunning.

## Required status check

- **Workflow:** `PR version`
- **Check/job name:** **`Minor version bump`**
- **Workflow file:** `.github/workflows/pr-version.yml`

The historical check name is retained so existing required-status-check settings
keep working. Despite its name, **`Minor version bump` accepts patch increases**.

After the workflow has run, maintainers can select **`Minor version bump`** from
GitHub Actions as a required status check in the ruleset or branch-protection rule
for `main` (the PR UI may group it as `PR version / Minor version bump`). Keep this
job name stable and unique. The workflow does **not** configure branch protection
or prevent merges until maintainers enable the required check themselves.

Enable **Require branches to be up to date before merging** alongside the required
check so a passing result cannot become stale after `main` advances. A push to
`main` does not itself trigger this PR-only workflow. Update the PR branch or
rerun the job to compare against a newly fetched main. If using a merge queue,
configure and validate the queue's required checks separately; this workflow is
scoped to `pull_request`, not `merge_group`.

The workflow runs on PR open, synchronize (new commits), reopen, edit (including
retargeting to main), and ready-for-review events. GitHub may require maintainer
approval before running a first-time contributor's fork workflow; do not replace
this with a privileged `pull_request_target` job to bypass that approval.

## Fork safety and maintenance

The job uses a GitHub-hosted ephemeral Ubuntu runner, explicit `contents: read`
permissions, a SHA-pinned checkout action, and `persist-credentials: false`. It
uses no repository secrets, writable tokens, caches, artifact publishing, or
release steps. No PR-controlled string is inserted directly into a shell command:
the exact PR SHA is passed through an environment variable and quoted. Git refs
are resolved without treating them as options; version files are read as data,
never evaluated as C or shell code. Python runs in isolated mode, using only the
standard library and the runner's installed Python 3.

Like other `pull_request` checks, the workflow and its helper/tests are part of the
reviewed change. Review modifications to these policy files carefully; the check
is not a substitute for trusted review or protected workflow files.

## Local verification

Python 3.10+ and Git are needed **only for these CI helper checks**, not for
building the Windows application. From the repository root:

```bash
python3 -I -B -m unittest discover -s .github/tests -p test_pr_version.py -v
git fetch origin main
python3 -I -B .github/scripts/check_pr_version.py --base-ref refs/remotes/origin/main --head-ref HEAD
```

Use `python` instead of `python3` if that is the installed interpreter name. The
checker reads committed Git objects; commit the intended version change before
running it. An uncommitted header edit cannot make a check pass. The unit and
real-Git integration tests cover patch/minor/major bumps, unchanged/lower
versions, strict parsing, missing/invalid files or refs on both sides, advanced
main versus the merge base, and an explicit PR head versus the checked-out result.
