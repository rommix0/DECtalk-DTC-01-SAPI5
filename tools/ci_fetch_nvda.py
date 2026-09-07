"""CI helper: download the newest NVDA installer for a release channel.

`check_nvda_api.py` needs a real NVDA tree -- `library.zip` plus the `*.pyd`
files that sit next to `nvda.exe` -- to diff the add-on's imports against. In
CI we obtain one by downloading the latest snapshot/release installer for a
channel and letting *that* installer build a portable copy
(`--create-portable-silent`, done in the workflow, not here).

This script is only the download half: it scrapes NV Access's Apache
directory listing for the channel, picks the newest build, and writes the
installer to `--out`.

Deliberately no third-party dependencies: this runs on the bare runner
Python before any `pip install`.

Usage:
    python tools/ci_fetch_nvda.py --channel alpha --out nvda-setup.exe
    python tools/ci_fetch_nvda.py --channel stable --out nvda-setup.exe
"""

from __future__ import annotations

import argparse
import re
import sys
import time
import urllib.request

_UA = "dtc-01 CI NVDA fetcher (+https://github.com/borris84/dectalk-dtc01)"


def _ver_key(text: str) -> tuple[int, ...]:
	"""'2026.1.1' -> (2026, 1, 1); 'beta3' -> (3,). Loose but monotonic."""
	return tuple(int(n) for n in re.findall(r"\d+", text))


# channel -> (listing URL, href regex; group(1)=filename, group(2)=sort key text,
#             key function over group(2))
_CHANNELS = {
	# nvda_snapshot_alpha-<build>,<hash>.exe -- <build> is a monotonic counter
	"alpha": (
		"https://download.nvaccess.org/snapshots/alpha/",
		re.compile(r'href="(nvda_snapshot_alpha-(\d+),[0-9a-fA-F]+\.exe)"'),
		lambda s: (int(s),),
	),
	# nvda_<version>beta<n>.exe
	"beta": (
		"https://download.nvaccess.org/releases/beta/",
		re.compile(r'href="(nvda_([0-9][0-9.]*(?:beta[0-9]+)?)\.exe)"'),
		_ver_key,
	),
	# nvda_<version>.exe
	"stable": (
		"https://download.nvaccess.org/releases/stable/",
		re.compile(r'href="(nvda_([0-9][0-9.]*)\.exe)"'),
		_ver_key,
	),
}


def _http_get(url: str, *, retries: int = 4, timeout: int = 120) -> bytes:
	last: Exception | None = None
	for attempt in range(1, retries + 1):
		try:
			req = urllib.request.Request(url, headers={"User-Agent": _UA})
			with urllib.request.urlopen(req, timeout=timeout) as resp:
				return resp.read()
		except Exception as exc:  # noqa: BLE001 - CI helper, any failure is retryable
			last = exc
			wait = 3 * attempt
			print(f"  GET {url} failed ({exc}); retry {attempt}/{retries} in {wait}s",
				  file=sys.stderr)
			time.sleep(wait)
	raise SystemExit(f"giving up on {url}: {last}")


def pick_newest(channel: str) -> tuple[str, str]:
	"""(filename, absolute URL) of the newest installer for the channel."""
	try:
		listing_url, pattern, keyfn = _CHANNELS[channel]
	except KeyError:
		raise SystemExit(f"unknown channel {channel!r}; "
						 f"choose from {', '.join(_CHANNELS)}")
	html = _http_get(listing_url).decode("utf-8", "replace")
	matches = list(pattern.finditer(html))
	if not matches:
		raise SystemExit(f"no installers matched at {listing_url} "
						 f"(listing format changed?)")
	best = max(matches, key=lambda m: keyfn(m.group(2)))
	filename = best.group(1)
	return filename, listing_url + filename


def main() -> int:
	ap = argparse.ArgumentParser(description=__doc__)
	ap.add_argument("--channel", default="alpha", choices=sorted(_CHANNELS),
					help="NVDA release channel (default: alpha)")
	ap.add_argument("--out", required=True,
					help="path to write the installer .exe to")
	ap.add_argument("--print-url", action="store_true",
					help="just print the resolved URL, do not download")
	args = ap.parse_args()

	filename, url = pick_newest(args.channel)
	print(f"newest {args.channel} installer: {filename}")
	print(f"url: {url}")
	if args.print_url:
		return 0

	blob = _http_get(url)
	if len(blob) < 20 * 1024 * 1024 or blob[:2] != b"MZ":
		raise SystemExit(f"downloaded {len(blob)} bytes from {url}; "
						 f"does not look like a Windows installer")
	with open(args.out, "wb") as fh:
		fh.write(blob)
	print(f"wrote {len(blob) / 1024 / 1024:.1f} MiB to {args.out}")

	# GitHub Actions: surface which build was tested.
	summary = __import__("os").environ.get("GITHUB_STEP_SUMMARY")
	if summary:
		with open(summary, "a", encoding="utf-8") as fh:
			fh.write(f"- NVDA `{args.channel}` build under test: **{filename}**\n")
	return 0


if __name__ == "__main__":
	raise SystemExit(main())
