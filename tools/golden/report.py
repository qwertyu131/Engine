"""Render a golden results.json (from `golden batch`) as one self-contained HTML report.

Usage:
    python tools/golden/report.py --results <dir>/results.json --out <dir>/report.html

Every image is embedded as a base64 data URI, so the HTML can be moved or opened anywhere,
offline (no CDN, no external files). Paths inside results.json are resolved relative to the
directory containing results.json (absolute paths are used as-is), matching `golden batch`.

Exit codes: 0 = report written, 2 = usage / IO / invalid results.json.
Stdlib only.
"""
from __future__ import annotations

import argparse
import base64
import html
import json
import math
import sys
from datetime import datetime, timezone
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
DASH = "—"

CSS = """
:root {
  color-scheme: light dark;
  --bg: #f6f7f9; --fg: #1b1f24; --muted: #5b6573; --card: #ffffff; --border: #d8dde3;
  --pass: #1a7f37; --pass-bg: #dafbe1; --fail: #cf222e; --fail-bg: #ffebe9;
  --ph-bg: #eef1f4; --ph-fg: #5b6573; --accent: #0969da;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0d1117; --fg: #e6edf3; --muted: #9198a1; --card: #161b22; --border: #30363d;
    --pass: #3fb950; --pass-bg: #12261e; --fail: #f85149; --fail-bg: #2d1214;
    --ph-bg: #1f242c; --ph-fg: #9198a1; --accent: #4493f8;
  }
}
* { box-sizing: border-box; }
body { margin: 0; padding: 16px; background: var(--bg); color: var(--fg);
       font: 14px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
header { display: flex; flex-wrap: wrap; gap: 12px 24px; align-items: baseline; margin-bottom: 16px; }
h1 { font-size: 20px; margin: 0; }
.meta { color: var(--muted); }
.totals b { font-variant-numeric: tabular-nums; }
.filters button { font: inherit; padding: 4px 12px; border: 1px solid var(--border); border-radius: 6px;
                  background: var(--card); color: var(--fg); cursor: pointer; }
.filters button[aria-pressed="true"] { border-color: var(--accent); color: var(--accent); }
body.only-fail section[data-status="PASS"] { display: none; }
section { background: var(--card); border: 1px solid var(--border); border-radius: 8px;
          padding: 12px 16px; margin-bottom: 16px; }
.case-head { display: flex; flex-wrap: wrap; gap: 8px 16px; align-items: center; margin-bottom: 8px; }
.case-head h2 { font-size: 16px; margin: 0; overflow-wrap: anywhere; }
.badge { font-weight: 700; padding: 1px 8px; border-radius: 999px; font-size: 12px; }
.badge.PASS { color: var(--pass); background: var(--pass-bg); }
.badge.FAIL { color: var(--fail); background: var(--fail-bg); }
dl { display: flex; flex-wrap: wrap; gap: 4px 20px; margin: 0 0 8px; }
dl div { display: flex; gap: 6px; }
dt { color: var(--muted); }
dd { margin: 0; font-variant-numeric: tabular-nums; }
.detail { color: var(--muted); margin: 0 0 8px; overflow-wrap: anywhere; }
.images { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 12px; }
figure { margin: 0; }
figcaption { color: var(--muted); font-size: 12px; margin-bottom: 4px; overflow-wrap: anywhere; }
figure img { display: block; width: 100%; height: auto; image-rendering: pixelated; cursor: zoom-in;
             border: 1px solid var(--border); background: var(--ph-bg); }
.placeholder { display: flex; flex-direction: column; justify-content: center; align-items: center;
               aspect-ratio: 1 / 1; border: 2px dashed var(--border); background: var(--ph-bg);
               color: var(--ph-fg); text-align: center; padding: 8px; overflow-wrap: anywhere; }
.placeholder strong { color: var(--fail); }
dialog { padding: 0; border: 1px solid var(--border); background: var(--card); max-width: 96vw; max-height: 96vh; }
dialog img { display: block; max-width: 94vw; max-height: 90vh; min-width: min(512px, 94vw);
             image-rendering: pixelated; cursor: zoom-out; }
dialog p { margin: 4px 8px; color: var(--muted); font-size: 12px; }
"""

JS = """
(function () {
  var dlg = document.getElementById('zoom');
  var big = document.getElementById('zoom-img');
  var cap = document.getElementById('zoom-cap');
  document.querySelectorAll('figure img').forEach(function (img) {
    img.addEventListener('click', function () {
      big.src = img.src; cap.textContent = img.alt; dlg.showModal();
    });
  });
  dlg.addEventListener('click', function () { dlg.close(); });
  var buttons = document.querySelectorAll('.filters button');
  buttons.forEach(function (b) {
    b.addEventListener('click', function () {
      document.body.classList.toggle('only-fail', b.dataset.filter === 'fail');
      buttons.forEach(function (o) { o.setAttribute('aria-pressed', String(o === b)); });
    });
  });
})();
"""


class ReportError(Exception):
    pass


def esc(value: object) -> str:
    return html.escape(str(value), quote=True)


def fmt_number(value: object, digits: int) -> str:
    if value is None:
        return DASH
    if value == "inf" or (isinstance(value, float) and math.isinf(value)):
        return "∞"
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return f"{value:.{digits}f}"
    return esc(value)


def resolve(base: Path, rel: object) -> Path | None:
    if not isinstance(rel, str) or not rel:
        return None
    p = Path(rel)
    return p if p.is_absolute() else base / p


def embed_png(path: Path | None) -> tuple[str | None, str]:
    """Returns (data URI or None, reason when None)."""
    if path is None:
        return None, "missing"
    try:
        data = path.read_bytes()
    except OSError:
        return None, "missing"
    if not data.startswith(PNG_SIGNATURE):
        return None, "unreadable (not a PNG)"
    return "data:image/png;base64," + base64.b64encode(data).decode("ascii"), ""


def figure(label: str, base: Path, rel: object, case_name: str) -> str:
    uri, why = embed_png(resolve(base, rel))
    shown = rel if isinstance(rel, str) else DASH
    if uri is None:
        return (f'<figure><figcaption>{esc(label)}</figcaption>'
                f'<div class="placeholder"><strong>{esc(label)} {esc(why)}</strong>'
                f'<span>{esc(shown)}</span></div></figure>')
    return (f'<figure><figcaption>{esc(label)} &middot; {esc(shown)}</figcaption>'
            f'<img src="{uri}" alt="{esc(case_name)} &ndash; {esc(label)}" loading="lazy"></figure>')


def render_case(case: dict, base: Path) -> str:
    if not isinstance(case, dict):
        raise ReportError("every entry in 'cases' must be an object")
    name = case.get("name", "?")
    status = "PASS" if case.get("status") == "PASS" else "FAIL"
    reason = case.get("reason", "")
    detail = case.get("detail") or ""
    metrics = [
        ("reason", esc(reason) if reason else DASH),
        ("flipMean", fmt_number(case.get("flipMean"), 6)),
        ("threshold", fmt_number(case.get("thresholdFlip"), 4)),
        ("psnr (dB)", fmt_number(case.get("psnr"), 2)),
        ("frameTimeMs", fmt_number(case.get("frameTimeMs"), 3)),
    ]
    dl = "".join(f"<div><dt>{esc(k)}</dt><dd>{v}</dd></div>" for k, v in metrics)
    images = (figure("Render", base, case.get("test"), str(name))
              + figure("Reference", base, case.get("ref"), str(name))
              + figure("FLIP diff", base, case.get("diff"), str(name)))
    detail_html = f'<p class="detail">{esc(detail)}</p>' if detail else ""
    return (f'<section data-case="{esc(name)}" data-status="{status}">'
            f'<div class="case-head"><span class="badge {status}">{status}</span><h2>{esc(name)}</h2></div>'
            f'<dl>{dl}</dl>{detail_html}<div class="images">{images}</div></section>')


def build_html(results: dict, base: Path) -> str:
    if not isinstance(results, dict) or not isinstance(results.get("cases"), list):
        raise ReportError("results.json must be an object with a 'cases' array")
    phase = results.get("phase", "?")
    cases = results["cases"]
    body = "".join(render_case(c, base) for c in cases)
    passed = sum(1 for c in cases if isinstance(c, dict) and c.get("status") == "PASS")
    failed = len(cases) - passed
    date = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    title = f"Golden report {phase}"
    return (
        "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        f"<title>{esc(title)}</title><style>{CSS}</style></head><body>"
        f"<header><h1>Golden report &middot; {esc(phase)}</h1>"
        f"<span class=\"meta\">{esc(date)}</span>"
        f"<span class=\"totals\"><b>{len(cases)}</b> cases &middot; <b>{passed}</b> PASS &middot; "
        f"<b>{failed}</b> FAIL</span>"
        "<span class=\"filters\"><button type=\"button\" data-filter=\"all\" aria-pressed=\"true\">All</button> "
        "<button type=\"button\" data-filter=\"fail\" aria-pressed=\"false\">Fail</button></span></header>"
        f"<main>{body}</main>"
        "<dialog id=\"zoom\"><img id=\"zoom-img\" alt=\"\"><p id=\"zoom-cap\"></p></dialog>"
        f"<script>{JS}</script></body></html>\n"
    )


def main(argv: list[str] | None = None) -> int:
    # Paths may be non-ASCII; never let the console/pipe codepage (e.g. cp1254) break output.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--results", required=True, type=Path, help="results.json from golden batch")
    parser.add_argument("--out", required=True, type=Path, help="report.html to write")
    try:
        args = parser.parse_args(argv)
    except SystemExit as e:
        return 0 if e.code == 0 else 2
    try:
        try:
            results = json.loads(args.results.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as e:
            raise ReportError(f"cannot read results '{args.results}': {e}") from e
        page = build_html(results, args.results.resolve().parent)
        tmp = args.out.with_name(args.out.name + ".part")
        try:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            tmp.write_text(page, encoding="utf-8")
            tmp.replace(args.out)
        except OSError as e:
            raise ReportError(f"cannot write '{args.out}': {e}") from e
    except ReportError as e:
        print(f"report: {e}", file=sys.stderr)
        return 2
    print(f"report: wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
