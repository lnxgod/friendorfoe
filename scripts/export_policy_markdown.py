#!/usr/bin/env python3
"""Render a readable GitHub copy of the canonical privacy HTML without maintaining two policies."""
import argparse
from html.parser import HTMLParser
from pathlib import Path


class PolicyMarkdown(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.in_body = False
        self.parts = []
        self.url = None

    def handle_starttag(self, tag, attrs):
        if tag == "body":
            self.in_body = True
        if not self.in_body:
            return
        if tag in {"html", "body", "main", "header", "section", "footer", "time"}:
            return
        if tag in {"h1", "h2", "h3", "p"}:
            self.parts.append("\n\n" + ("#" * int(tag[1]) + " " if tag != "p" else ""))
        elif tag == "a":
            self.url = dict(attrs)["href"]
            if not self.url.startswith("https://"):
                raise ValueError("Policy link must use HTTPS")
            self.parts.append("[")
        else:
            raise ValueError(f"Unsupported policy element: {tag}")

    def handle_endtag(self, tag):
        if not self.in_body:
            return
        if tag == "body":
            self.in_body = False
        elif tag == "a":
            self.parts.append(f"]({self.url})")
            self.url = None
        elif tag in {"h1", "h2", "h3", "p"}:
            self.parts.append("\n\n")

    def handle_data(self, text):
        if self.in_body:
            self.parts.append(text)

    def render(self):
        return "\n\n".join(line.strip() for line in "".join(self.parts).splitlines() if line.strip()) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=["android", "iphone"])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    source = {"android": "android/app/src/main/res/raw/privacy_policy.html", "iphone": "docs/iphone-privacy.html"}[args.platform]
    target = repo / f"docs/{args.platform}-privacy-policy.md"
    renderer = PolicyMarkdown()
    renderer.feed((repo / source).read_text())
    renderer.close()
    expected = renderer.render()
    if args.check:
        if not target.is_file() or target.read_text() != expected:
            parser.exit(1, f"{target.name} is stale; re-run without --check\n")
    else:
        target.write_text(expected)


if __name__ == "__main__":
    main()
