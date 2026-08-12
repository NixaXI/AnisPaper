# Contributing to AnisPaper ✦

Forks, issues and pull requests are welcome.

## Before opening a PR

1. Fork the repository.
2. Create a focused branch.
3. Keep unrelated cleanup out of the same PR.
4. Build the affected targets.
5. Run relevant tests.
6. Test on a real Plasma output when touching rendering, SHM, QML or monitor mapping.
7. Explain what changed and how you verified it.

## AI-assisted contributions

AI-assisted development is allowed.

The contributor is still responsible for:

- understanding the submitted code,
- reviewing the diff,
- testing it,
- ensuring no secrets/private data are included,
- checking license compatibility,
- avoiding copied code they do not have the right to submit.

You do not need to apologize for using AI.

You do need to be able to explain and maintain the code you submit.

## Coding expectations

- Prefer small, reviewable changes.
- Do not hide failures behind silent fallbacks.
- Renderer failures must not take `plasmashell` down.
- Do not hardcode a developer home directory, Steam library, output name or Workshop ID.
- Keep logs useful and bounded.
- Never commit build output or Steam Workshop content.
- Preserve third-party licenses and attribution.

## Rendering bug reports

Please include:

```text
Plasma version:
Session: Wayland / X11
GPU:
Driver:
Outputs:
Wallpaper type:
Wallpaper ID:
Does preview.frame look correct?:
Does the renderer stay alive?:
```

## Licensing

By submitting a contribution, you confirm you have the right to submit it under the repository license.

Do not paste third-party code of unknown origin into a PR.
