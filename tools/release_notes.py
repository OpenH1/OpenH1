#!/usr/bin/env python3
"""A release's notes, from the pull requests merged since the last release:
the highlights What's new shows over the main menu after an update
(port/linux/src/whats_new.c), which the release's builds embed.

    python tools/release_notes.py highlights [COMMIT]   # up to five lines, from git's history
    python tools/release_notes.py whats-new BUILD PAGE < highlights   # the notes file's text

The workflow's changes job (which has the history) finds the highlights of a
build of master: the titles of the pull requests merged since the newest
build-<n> tag the commit holds (the last release), newest first, but dev's
merges into master and those about tests, documentation or CI alone, each cut
to fit the card. Each build of the run (tools/ci_build.py, with
CI_WHATS_NEW) then writes them as port/assets/menus/whats_new.txt

    build 2290
    page https://github.com/OpenH1/goath1/releases/tag/build-2290
    - Add a Doubles matchmaking playlist
    - ...

which tools/embed_assets.py embeds with the menus' files. A build without it
(a local one, a branch's) shows no card. The release lists every change on
its page (the workflow's release job), which READ ALL opens.
"""
import re
import subprocess
import sys
from pathlib import Path
from typing import List, Optional

ROOT = Path(__file__).resolve().parent.parent
# the notes' file, beside the menus' files it is embedded with (git ignores it)
NOTES = ROOT / "port/assets/menus/whats_new.txt"
# the card's lines, and the longest a line may be (its width, in the small font)
MAXIMUM_LINES = 5
LINE_LENGTH = 58
# a pull request merged ("Merge pull request #12 from owner/branch"), and the
# branches whose merges are not changes of their own (dev into master)
MERGE = re.compile(r"^Merge pull request #\d+ from [^/\s]+/(\S+)")
NOT_CHANGES = {"dev", "master"}
# titles about the project's own work, not the game's
CHORES = re.compile(r"\b(tests?|harness|CI|docs?|documentation|README|workflows?)\b|\.md\b|\.py\b|\(#\d+\)$",
                    re.IGNORECASE)


def clip(title: str, length: int = LINE_LENGTH) -> str:
    """the title cut to length at a word, with an ellipsis where it was cut"""
    title = " ".join(title.split())
    if len(title) <= length:
        return title
    cut = title[:length - 3].rsplit(" ", 1)[0].rstrip(" ,;:-")
    return cut + "..."


def highlights(messages: List[str]) -> List[str]:
    """the highlights of merge commits' messages, newest first: each merged
    pull request's title (its message's first line after the merge's),
    dev's merges and chores left out, each once"""
    lines: List[str] = []
    for message in messages:
        parts = [line.strip() for line in message.strip().splitlines() if line.strip()]
        merge = MERGE.match(parts[0]) if parts else None
        if not merge or merge.group(1) in NOT_CHANGES or len(parts) < 2:
            continue
        title = parts[1]
        if CHORES.search(title) or not title.isprintable():
            continue
        line = clip(title)
        if line not in lines:
            lines.append(line)
        if len(lines) == MAXIMUM_LINES:
            break
    return lines


def git(*arguments: str) -> str:
    return subprocess.run(["git", *arguments], cwd=ROOT, capture_output=True, text=True, check=True,
                          timeout=60).stdout


def previous_release(commit: str) -> Optional[str]:
    """the newest build-<n> tag the commit holds (not the commit's own)"""
    tags = [tag for tag in git("tag", "--list", "build-*", "--merged", commit).split()
            if re.fullmatch(r"build-\d+", tag) and git("rev-list", "-n", "1", tag).strip() != git(
                "rev-parse", commit).strip()]
    return max(tags, key=lambda tag: int(tag.split("-")[1]), default=None)


def commit_highlights(commit: str) -> List[str]:
    since = previous_release(commit)
    if not since:
        return []
    # (every merge since, newest first: the pull requests are merged into
    # dev, and dev into master)
    log = git("log", "--merges", "--format=%B%x00", f"{since}..{commit}")
    messages = [message for message in log.split("\0") if message.strip()]
    return highlights(messages)


def whats_new(build: int, page: str, lines: List[str]) -> str:
    """the notes' text (whats_new.h's whats_new_parse); "" without lines"""
    lines = [clip(line) for line in lines if line.strip() and line.isprintable()][:MAXIMUM_LINES]
    if build <= 0 or not lines:
        return ""
    text = f"build {build}\n"
    if re.fullmatch(r"https://[^\s\"'<>]+", page):
        text += f"page {page}\n"
    return text + "".join(f"- {line}\n" for line in lines)


def write(build: int, page: str, lines: List[str], path: Path = NOTES) -> bool:
    """the notes written for the build to embed (True), else any left from
    before removed"""
    text = whats_new(build, page, lines)
    if not text:
        path.unlink(missing_ok=True)
        return False
    path.write_text(text, encoding="utf-8")
    return True


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "highlights":
        commit = sys.argv[2] if len(sys.argv) > 2 else "HEAD"
        for line in commit_highlights(commit):
            print(line)
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "whats-new":
        sys.stdout.write(whats_new(int(sys.argv[2]), sys.argv[3], sys.stdin.read().splitlines()))
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
