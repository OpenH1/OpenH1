# Contributing to OpenH1

Pull requests are welcome, from small fixes to whole renderer features. We
review them promptly, and merge what is ready.

1. Fork the repository and branch from `master`.
2. Read [AGENTS.md](AGENTS.md): the layout, the build, the conventions (the
   game's sources are C89, `gnu89`), the tests, and what must stay
   deterministic.
3. Open a pull request. GitHub Actions builds it for Android, Windows, macOS
   and the Linux builds, and runs the tests.
4. Say what you tested and how, and what still needs a device to check.

Fixes waiting in [OpenCE](https://github.com/OpenCommunityEdition/OpenCE) are
welcome here too: open them here as well, or tell us about them. We bring good
ones in with their authorship kept and credit given.

A change to what the game sends over the network, or to how it calculates
the game, must keep network play compatible: refer to "Contributing" and
"Testing" in [README.md](README.md).
