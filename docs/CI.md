# CI and releases

Use GitHub Actions to check source changes and prepare release candidates. A successful run
establishes what the host tests and build tools checked. Game behavior needs its own emulator
or console results before a maintainer qualifies a release.

## Environment

| Tool | Version | Source |
| --- | --- | --- |
| Python | 3.11.9 | [Python releases](https://www.python.org/downloads/release/python-3119/) |
| Node.js | 24.14.0 | [Node.js release](https://nodejs.org/download/release/v24.14.0/) |
| LLVM | 22.1.1 | [LLVM releases](https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.1) |
| PyYAML | 6.0.3 | `requirements-dev.txt` |
| Unicorn | 2.1.4 | `requirements-dev.txt` |
| Docs packages | Locked | `site/package-lock.json` |

Install Python packages with `python -m pip install -r requirements-dev.txt` and docs packages
with `npm --prefix site ci`. Put LLVM's `clang`, `clang++`, and `ld.lld` on `PATH`.
[CONTRIBUTING.md](../CONTRIBUTING.md#setup) gives the local setup steps.

## Checks

Pull requests and pushes to `main` run host checks and ARM64 builds on a Windows runner, and build the
docs on an Ubuntu runner. The docs build generates the API reference and checks links.
Generated files stay in build outputs. Dependency updates arrive as pull requests so the same
checks can review them.

Dependency updates are grouped by ecosystem. Branch pushes use the pull request's checks,
and a new ordinary CI run cancels an older run for the same ref to avoid duplicate builds.

Run `python tools/check.py` before you commit. To reproduce the stricter CI gate, run:

```sh
python tools/check.py --ci --report build/ci/checks.json
```

The runner checks required tools before it runs the full suite. The Windows job requires every
test to run. On another host, only the named Windows TLS check may be skipped, and the JSON
report records that exception. Any other skipped test fails CI. Windows TLS checks need
OpenSSL. The resolver checks Git for Windows and `PATH`, or uses the file named by
`TSUCHI_OPENSSL` when that override is set.

A clean checkout compiles several host fixtures and both ARM64 modules, so the complete gate
can take tens of minutes. The host job allows 90 minutes for checks and candidate packaging.

To build the module or docs separately, use `python tools/build_module.py` and
`npm --prefix site run build`. Set `TSUCHI_BUILD_JOBS` to a positive integer when you need to
limit concurrent compilations.

## Candidates

CI builds the production module and its matching patch, then packages the runtime and SDK.
The packaging tools verify the module, patch, API identity, and archive contents. Candidate
archives use `--prerelease` while the API lock remains unreleased. A self-check module is for
game tests and isn't included as the runtime people install.

From a clean, committed checkout, build and package the same candidate files locally:

```sh
python tools/ci_package.py --build --out build/ci/candidate
```

Download `tsuchi-candidate` from the workflow run for the source revision you want to test.
`tsuchi-checks` holds the reports, and `tsuchi-docs` holds the built docs. Keep the module and
patch from that build together. Preserve their checksums and the source revision with your
test results so a maintainer can associate a result with the files tested.

## Releases

A `v*` version tag or a manual run of `release.yml` with an existing tag can prepare a draft
GitHub prerelease. The release workflow reuses `ci.yml` to check, build, and package that tag
before it attaches archives and checksums to the draft. For a local tagged candidate, add
`--tag v0.2.0-rc.1` to the packaging command, using the tag you checked out. A maintainer reviews
the draft and publishes it after the required game checks.

The workflow doesn't deploy the docs site or turn a successful build into a game qualification.
The website's download packaging remains a separate step, with the archive hashes supplied to
`tools/package_site.py`. That command enforces the repository's release identity rules.

[Discord announcements](DISCORD.md) post the release notes after the reviewed draft is published.
Configure the webhook when the release channel is ready.

## Build profiles

`main` supports Pikmin 4 1.1.1 and 2.0.0 for Switch, Pikmin 4 Nintendo Switch 2 Edition, and the
Switch 2 demo. The framework code is shared, and each build has a profile: 1.1.1's in
`game/table.yaml`, the others' in `game/builds/`. A release packages one module with every
build's patch, and the host tests check every profile. Record emulator and console results
separately, including any missing services or unfinished checks. A shader archive, raw address,
or compiled game-memory layout for one build needs review before another build can use it.

## Repository settings

After the repository is on GitHub, enable Actions and require `Host checks and ARM64 builds`
and `Documentation` from `ci.yml` on pull requests to `main`. Allow the release workflow to write repository
contents so it can create draft releases. Keep ordinary check jobs read-only, and use the
built-in `GITHUB_TOKEN` for releases. No personal access token or game-data secret is required.

Branch rules and Actions settings belong to the GitHub repository and aren't configured by
these source files. Select required checks after the first workflow run gives GitHub their
names. Review dependency pull requests with the same branch rules as other changes.
