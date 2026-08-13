# Contributing

Thanks for helping improve Xtream Player.

## Development setup

Install the Fedora dependencies listed in the README, then build with:

```sh
meson setup build
meson compile -C build
```

Use `meson setup --wipe build` after changing build options or dependencies.

## Pull requests

- Keep changes focused and explain the user-facing impact.
- Build with the default warning level before submitting.
- Never commit provider credentials, playlist URLs, or cached API responses.
- Include reproduction steps for bug fixes.
- Follow the existing C style: four-space indentation and descriptive names.

By contributing, you agree that your contribution is licensed under the MIT
License included with this repository.
