# Contributing to Ultramaster KR-106

Thanks for your interest in contributing! Here's how to get started.

## Reporting Bugs

Use the [Bug Report](https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/new?template=bug_report.yml) template. Include your OS, DAW, plugin format, and steps to reproduce.

## Suggesting Features

Use the [Feature Request](https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/new?template=feature_request.yml) template.

## Pull Requests

1. Fork the repo and create a branch from `main`.
2. Make sure the project builds on your platform (`make build` or `cmake --build build`).
3. Keep changes focused — one fix or feature per PR.
4. Write clear commit messages describing what changed and why.

## Code Style

- C++17
- Member variables prefixed with `m` (e.g. `mSampleRate`)
- Constants prefixed with `k` (e.g. `kNumVoices`)
- Header-only DSP classes in `Source/DSP/`
- Header-only controls in `Source/Controls/`

## DSP Changes

If your change affects the sound engine, please describe the signal-processing
rationale in the PR description. Reference circuit schematics or measurements
where applicable.

## License

By contributing, you agree that your contributions will be licensed under the
[GNU General Public License v3.0](LICENSE).
