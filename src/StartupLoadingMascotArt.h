#pragma once

namespace HarkenSearch {

inline constexpr wchar_t kStartupLoadingMascotArtwork[] = LR"MASCOT(
<svg class="startup-loading-mascot" viewBox="0 0 50 72" preserveAspectRatio="xMidYMid meet" aria-hidden="true" focusable="false" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="startup-loading-mascot-body-paint" x1="1605.51" y1="806.498" x2="1605.51" y2="990.57" gradientUnits="userSpaceOnUse"><stop stop-color="#434343" /><stop offset="0.5" stop-color="#2C2C2C" /><stop offset="1" stop-color="#FF861D" /></linearGradient>
    <linearGradient id="startup-loading-mascot-antenna-paint" x1="1581.51" y1="748.719" x2="1581.51" y2="798.921" gradientUnits="userSpaceOnUse"><stop stop-color="#4D4D4D" /><stop offset="0.505538" stop-color="#141414" /><stop offset="1" stop-color="#FF861D" /></linearGradient>
    <linearGradient id="startup-loading-mascot-warm-paint" x1="1581.51" y1="748.719" x2="1581.51" y2="798.921" gradientUnits="userSpaceOnUse">
      <stop stop-color="#7C5133" stop-opacity="0.8"/>
      <stop offset="0.505538" stop-color="#6F4426"/>
      <stop offset="1" stop-color="#FFB62F"/>
    </linearGradient>
    <path d="M1587.71 790.08C1590.2 793.276 1594.57 794.916 1597.25 791.882C1598.66 790.289 1599.83 788.48 1600.7 786.51C1602.58 782.258 1602.98 777.501 1601.85 772.993C1600.71 768.486 1598.1 764.487 1594.43 761.633C1590.76 758.78 1586.24 757.235 1581.6 757.244C1576.95 757.254 1572.44 758.817 1568.78 761.685C1565.12 764.554 1562.53 768.563 1561.41 773.075C1560.29 777.587 1560.72 782.343 1562.61 786.587C1563.47 788.502 1564.6 790.263 1565.97 791.819C1568.71 794.946 1573.18 793.238 1575.89 790.08C1576.49 789.377 1577 788.468 1577.4 787.555C1578.4 785.311 1576.92 782.881 1575.92 780.639C1575.35 779.363 1575.22 777.934 1575.56 776.577C1575.89 775.221 1576.67 774.016 1577.77 773.153C1578.87 772.291 1580.23 771.821 1581.63 771.818C1583.02 771.816 1584.38 772.28 1585.48 773.138C1586.59 773.996 1587.37 775.198 1587.71 776.553C1588.05 777.908 1587.93 779.338 1587.37 780.616C1586.4 782.812 1585.11 785.288 1586.16 787.451C1586.61 788.4 1587.15 789.352 1587.71 790.08Z" id="startup-loading-mascot-arch" />
  </defs>
  <g transform="translate(2.2 17.15) scale(0.301) translate(-1531 -806.795)">
    <path class="startup-loading-mascot-body" d="M1657.9 806.795C1670.1 806.795 1680 816.69 1680 828.896V959.207C1680 971.413 1670.1 981.309 1657.9 981.309H1645.26C1641.39 981.309 1637.91 978.957 1636.47 975.368L1633.45 967.858C1632.01 964.27 1628.53 961.918 1624.66 961.918H1587.02C1583.3 961.918 1579.93 964.098 1578.39 967.489L1574.66 975.738C1573.13 979.129 1569.75 981.309 1566.03 981.309H1553.1C1540.9 981.309 1531 971.413 1531 959.207V828.896C1531 816.69 1540.9 806.795 1553.1 806.795H1657.9Z" fill="url(#startup-loading-mascot-body-paint)" />
  </g>
  <g class="startup-loading-mascot-antenna">
    <g transform="translate(2.2 17.15) scale(0.301) translate(-1531 -806.795)">
      <use href="#startup-loading-mascot-arch" fill="url(#startup-loading-mascot-antenna-paint)"/>
      <use class="startup-loading-mascot-antenna-warmth" href="#startup-loading-mascot-arch" fill="url(#startup-loading-mascot-warm-paint)" opacity="0"/>
    </g>
  </g>
  <g class="startup-loading-mascot-eyes">
    <g transform="translate(2.2 17.15) scale(0.301) translate(-1531 -806.795)">
      <rect x="1574" y="854" width="31" height="17" rx="6.73259" fill="#F1F1F1" />
      <rect x="1630" y="854" width="31" height="17" rx="6.73259" fill="#F1F1F1" />
    </g>
  </g>
</svg>
)MASCOT";

}
