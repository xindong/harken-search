"""Embed the offline updater page and exact official PNGs into its executable."""
import argparse
import base64
from pathlib import Path

def render(root: Path) -> str:
    page = (root / 'src/bootstrap/BootstrapProgressPage.html').read_text(encoding='utf-8')
    for token, relative in {
        '@SEARCH_ICON@': 'assets/icons/source/harken-app-icons/Search/Search transparent 64.png',
        '@WORDMARK_BLACK@': 'assets/logos/brand/harken-search-menu-wordmark-black.png',
        '@WORDMARK_WHITE@': 'assets/logos/brand/harken-search-menu-wordmark-white.png',
    }.items():
        page = page.replace(token, 'data:image/png;base64,' + base64.b64encode((root / relative).read_bytes()).decode('ascii'))
    return page

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--html', action='store_true')
    args = parser.parse_args()
    page = render(args.root)
    if not args.html:
        # Separate expressions avoid MSVC's 64 KiB string-literal limit.
        parts = ['#pragma once\n#include <string>\nnamespace aris::bootstrap {\ninline std::string BootstrapProgressPageHtml() {\nstd::string page;\n']
        for offset in range(0, len(page), 8000):
            parts.append('page += R"HARKEN_PAGE(' + page[offset:offset+8000] + ')HARKEN_PAGE";\n')
        parts.append('return page;\n}\n}\n')
        page = ''.join(parts)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(page, encoding='utf-8')
