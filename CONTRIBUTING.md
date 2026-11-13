# Contributing

Thanks for your interest in Nebula. Bug reports, fixes and focused improvements are welcome.

## Before you start

- Search the [issues](https://github.com/lcoutodemos/Nebula/issues) first. For anything larger than a small fix, open an issue to
  discuss the change before you write code.
- Read [docs/building.md](docs/building.md) to set up your machine and
  [docs/architecture.md](docs/architecture.md) to find your way around the code.

## Making a change

1. Fork the repository and create a branch from `main`.
2. Keep the change focused. Match the style of the surrounding code; `.editorconfig` covers the basics.
3. Build and test locally:

   ```powershell
   .\build.ps1 -Test
   ```

   This builds the native renderer and runs its GPU smoke tests, builds the app and the tools, and
   runs the core algorithm checks. See [docs/testing.md](docs/testing.md) for the other test suites.
4. If you change what the user sees, include before and after screenshots in the pull request.
5. Update the documentation in `docs/` when you change behaviour it describes.

## Privacy

Nebula keeps every photo, index and query on the PC. Contributions must not add network calls, telemetry
or analytics to the app. Never commit photos, libraries, model packs, logs or anything else generated
from your own data; test with synthetic fixtures (`tests/create_fixtures.py`) or openly licensed images.

## License of contributions

Nebula is licensed under the [Apache License 2.0](LICENSE). Unless you say otherwise, any contribution you
submit is licensed under the same terms, as section 5 of the license describes. Only submit work you have
the right to license this way.

## Code of conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md).
