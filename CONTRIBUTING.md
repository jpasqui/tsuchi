# Contribute to Tsuchi

Change the API in
`schema/` or its generators, since headers, bindings, and reference pages are generated.

## Setup

Install Python 3.11.9, Node.js 24.14.0, and LLVM 22.1.1. Put `clang`, `clang++`, and
`ld.lld` on your `PATH`. The module build needs LLVM's AArch64 target support. The version files
in the repository record the Python and Node.js versions used by CI.

Create a Python environment and install the development dependencies:

```sh
python -m venv .venv
```

On Windows PowerShell, activate it with `.venv\Scripts\Activate.ps1`. On Linux or macOS,
run `source .venv/bin/activate`. Then install the Python dependencies and the docs packages:

```sh
python -m pip install -r requirements-dev.txt
npm --prefix site ci
```

`requirements-dev.txt` pins the Python packages, and `site/package-lock.json` pins the docs
packages. See [CI](docs/CI.md) for the automated checks and release process.

## Checks

Run the repository gate before a commit:

```sh
python tools/check.py
```

To run one Python test module while you work, use its module name. For example:

```sh
python -m unittest discover -s tests -p test_package_release.py -v
```

To build the module and its patch, run:

```sh
python tools/build_module.py
```

To build the docs and check their links, run:

```sh
npm --prefix site run build
```

Add checks to the existing test suite for behavior you change. In your pull request, say which
checks ran and whether they used a host fixture, an emulator, or a console. Host checks and an
ARM64 build don't establish that a change works in the game.

## Pull requests

Keep each pull request focused on one change, explain the resulting behavior, and link any
relevant issue. Preserve unrelated changes when you share a working directory with another
contributor. Use a separate branch or worktree for independent work.

The current `main` supports Pikmin 4 1.1.1 for Switch. Work on other game builds stays on its
development branch until its code, build profiles, checks, and documentation are ready to merge.
A port changes shared framework code, so its review must include regressions on existing builds.
See [Build profiles](docs/CI.md#build-profiles) for the integration requirements.

## Content and license

Contribute source, tests, and documentation under the [MIT license](LICENSE). Preserve the
licenses and notices of dependencies under `third_party/`; the repository's MIT license doesn't
replace those licenses. Review [third-party notices](tools/Tsuchi-ThirdPartyNotices.txt) when you
change a dependency.

Keep game binaries, assets, saves, keys, local paths, and private research out of commits and
release archives. Use small authored fixtures for reproducible tests, and share logs with
private information removed. The module build and automated checks don't require game files.
