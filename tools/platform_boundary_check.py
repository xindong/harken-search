#!/usr/bin/env python3
"""Enforce the Stage 6 platform-boundary migration ledger."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import Counter
from dataclasses import dataclass, replace
from datetime import date
from pathlib import Path
from typing import Iterable


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".mm"}
ALLOWED_OWNER_SLICES = {
    "S6-02", "S6-03", "S6-04", "S6-05A", "S6-05B", "S6-05C", "S6-06", "S6-07A", "S6-07B",
}
DEFAULT_LEDGER = Path("docs/plans/refactor/stage-6-platform-migration-ledger.json")
DEFAULT_EXCEPTIONS = Path("docs/plans/refactor/stage-6-platform-audited-exceptions.json")
PLATFORM_ADAPTER_PATH = "src/bootstrap/WindowsBootstrapFilesystemWin32.cpp"
PLATFORM_ADAPTER_OWNER = "BOOTSTRAP-FILESYSTEM"
PLATFORM_ADAPTER_EVIDENCE = (
    "docs/plans/refactor/stage-6-bootstrap-filesystem-adapter-audit.md"
)
RUNTIME_PLATFORM_ADAPTER_PATH = "src/bootstrap/WindowsBootstrapRuntimeWin32.cpp"
RUNTIME_PLATFORM_ADAPTER_OWNER = "BOOTSTRAP-RUNTIME"
RUNTIME_PLATFORM_ADAPTER_EVIDENCE = (
    "docs/plans/refactor/stage-6-bootstrap-runtime-adapter-audit.md"
)
BOOTSTRAP_EXIT_PLATFORM_ADAPTER_PATH = "src/bootstrap/HarkenSearchBootstrapExitExplanation.cpp"
BOOTSTRAP_EXIT_PLATFORM_ADAPTER_OWNER = "BOOTSTRAP-EXIT"
BOOTSTRAP_EXIT_PLATFORM_ADAPTER_EVIDENCE = (
    "docs/plans/refactor/stage-6-bootstrap-exit-explanation-audit.md"
)
UPDATE_APPLICATION_PLATFORM_ADAPTER_PATH = "src/update/HarkenSearchUpdateComposition.cpp"
UPDATE_APPLICATION_PLATFORM_ADAPTER_OWNER = "UPDATE-APPLICATION"
UPDATE_APPLICATION_PLATFORM_ADAPTER_EVIDENCE = (
    "docs/plans/refactor/stage-6-update-application-adapter-audit.md"
)
UPDATE_CHECK_HTTPS_PLATFORM_ADAPTER_PATH = (
    "src/update/UpdateCheckHttpsTransport.cpp"
)
UPDATE_CHECK_HTTPS_PLATFORM_ADAPTER_OWNER = "UPDATE-CHECK-HTTPS"
UPDATE_CHECK_HTTPS_PLATFORM_ADAPTER_EVIDENCE = (
    "docs/plans/refactor/stage-6-update-check-https-adapter-audit.md"
)
PLATFORM_ADAPTER_RULES = frozenset({"native-include", "native-symbol"})
UPDATE_APPLICATION_PLATFORM_ADAPTER_RULES = frozenset({
    "filesystem-io", "native-include", "native-symbol",
})

# These are policy identities, not scanner exclusions. The scanner still reports
# every finding; only the exact reviewed fingerprints below may be classified.
APPROVED_PLATFORM_ENTRY_FINGERPRINTS = frozenset({
    "src/ConsentManager.cpp|native-include|windows.h|d395cd89463e8f58|1",
    "src/WindowsMain.cpp|native-include|WebView2.h|b2137ebe9b764182|1",
    "src/WindowsMain.cpp|native-include|dbghelp.h|0c862e39dfa4b607|1",
    "src/WindowsMain.cpp|native-include|ole2.h|d9e53221b9eff462|1",
    "src/WindowsMain.cpp|native-include|shellapi.h|a3407e692c94a8de|1",
    "src/WindowsMain.cpp|native-include|windows.h|d395cd89463e8f58|1",
    "src/WindowsMain.cpp|native-include|wrl.h|9a6f80392d3201fd|1",
    "src/coverwall_lab/CoverWallLabMain.cpp|native-include|windows.h|d395cd89463e8f58|1",
    "src/ui/dock/DockCaptionWndProc.h|native-include|windows.h|d395cd89463e8f58|1",
    "src/main.cpp|native-include|commdlg.h|8ef3a77175d94645|1",
    "src/main.cpp|native-include|ole2.h|d9e53221b9eff462|1",
    "src/main.cpp|native-include|shlobj.h|2d3e62b5423f2cb0|1",
    "src/main.cpp|native-include|windows.h|d395cd89463e8f58|1",
})
APPROVED_PLATFORM_ADAPTER_FINGERPRINTS = frozenset({
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-include|Windows.h|3d20652c81953438|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-include|bcrypt.h|ec818221406cb598|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|BCryptCloseAlgorithmProvider|0249946307494502|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|BCryptCreateHash|44e3ae58ef7fc40f|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|BCryptDestroyHash|b7f3ff86f901983a|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|BCryptFinishHash|2864e4843c26384a|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|BCryptGetProperty|f453c3a473c43fa8|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|BCryptHashData|e87d735a72727047|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|BCryptOpenAlgorithmProvider|10dafcc6cbd90e67|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|CloseHandle|251f0de6d73f67b4|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|CloseHandle|2f761bc03f9d6b94|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|CloseHandle|3173ce2c50b9aeb9|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|CreateFileW|36125086295918db|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|CreateFileW|5611d8bbec4137fe|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|CreateFileW|641a357e0f2e6f3e|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|DuplicateHandle|277d23c5db46ad09|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|EqualSid|3bd4dddc2542cd5e|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|EqualSid|ade9b29d3f25fc3b|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|EqualSid|b7d1da815f71bf07|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|EqualSid|d084f69a161f5d65|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|EqualSid|d73b5b5f0f6c4815|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|EqualSid|dcfb07ff8378356f|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|FlushFileBuffers|4d7135c3119e6a78|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|FlushFileBuffers|78eba01c6e550128|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|FlushFileBuffers|78eba01c6e550128|2',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|FlushFileBuffers|8eb3d1aedf399654|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|FlushFileBuffers|fe91fad181aa91ec|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetCurrentProcess|277d23c5db46ad09|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetCurrentProcess|277d23c5db46ad09|2',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFileInformationByHandleEx|27f74d3021a12e84|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFileInformationByHandleEx|298c5d5cd5274c7b|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFileInformationByHandleEx|2a980c68a43692b9|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFileInformationByHandleEx|6cb3404a7c9abc90|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFileInformationByHandleEx|817e237badf4f9f0|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFileInformationByHandleEx|817e237badf4f9f0|2',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFileSizeEx|71f1f46c25a0ccad|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFileType|d740d5ef5cf1ce55|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFinalPathNameByHandleW|3284586eb5b349ec|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFinalPathNameByHandleW|965c41ba9fe90fbf|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFullPathNameW|53539e7e8e6eccab|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetFullPathNameW|8f3013b7b63703b7|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|4705fdfab6477259|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|4d0b11686c869fdb|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|4d0b11686c869fdb|2',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|556d9d55df1c2600|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|5e329acee6b23214|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|71f1f46c25a0ccad|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|87e1225402594918|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|a5fe34004900e804|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|a5fe34004900e804|2',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|a5fe34004900e804|3',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|a5fe34004900e804|4',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|a5fe34004900e804|5',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|a9c9fd26f7475463|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|aac903539891a912|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|10',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|11',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|12',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|13',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|14',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|15',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|2',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|3',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|4',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|5',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|6',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|7',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|8',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|b1e89e1087a39383|9',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|c402e99640e96456|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|d6183be8f4af725e|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLastError|f8af3cd7105bedf0|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetLengthSid|3af76e6240d025b2|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetModuleFileNameW|45fc316ac06f83aa|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetTokenInformation|50442f89ac0671e4|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|GetTokenInformation|839d4f29f99c1f80|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|IsValidSid|342419a282a52fb2|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|IsValidSid|3af76e6240d025b2|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|IsValidSid|4f3ff2559e95b235|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|IsValidSid|ecb060aaa89da8a3|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|LocalFree|48b1efb582389847|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|MultiByteToWideChar|759bda65150f6104|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|MultiByteToWideChar|7afd2c12aa1cd3c6|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|OpenProcessToken|4d6729b6e781ad34|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|ReadFile|2b85da44659bf1b4|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|ReadFile|e14fa68c0fd887b9|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|ReadFile|ea7ee11c82c50b70|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|SetFilePointerEx|d2266bd67db6684b|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|SetFilePointerEx|d350d4b9cd126b82|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|SetFilePointerEx|d350d4b9cd126b82|2',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|SetLastError|b0980f149dc31d86|1',
    'src/bootstrap/WindowsBootstrapFilesystemWin32.cpp|native-symbol|WriteFile|655ae88398f96575|1',
})
APPROVED_RUNTIME_PLATFORM_ADAPTER_FINGERPRINTS = frozenset({
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-include|knownfolders.h|d6c6ac6448757e97|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-include|objbase.h|cd73b8c775fbde36|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-include|shellapi.h|a3407e692c94a8de|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-include|shlobj.h|2d3e62b5423f2cb0|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-include|softpub.h|87cc71ffa7624db1|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-include|wincrypt.h|79ec836fab26ec25|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-include|windows.h|d395cd89463e8f58|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-include|wintrust.h|2a804c471cf6c52e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|AddAccessAllowedAce|27bd551df12d7ba1|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|AssignProcessToJobObject|d663ab9142ce57a3|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CancelIoEx|4100069cd79de606|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CancelIoEx|4100069cd79de606|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CancelIoEx|4100069cd79de606|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CertGetCertificateContextProperty|5c3776917e06ee41|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|0d5d50cca3629ffb|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|10',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|11',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|12',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|13',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|14',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|15',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|16',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|5',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|6',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|7',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|8',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|11c33203065d1392|9',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|1410861f9cd08574|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|1a6ab34744262f74|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|1f4324d453581f7e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|29932aab2ed1e325|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|2af65554a92764fe|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|2b054e65b6bbd075|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|2fed500240c725cb|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|338ff3e3a22c0e59|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|3debc3d65da51169|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|3f8c0c1a242d10c7|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|48e0aa60754ff0fc|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|48e0aa60754ff0fc|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|4ce2071783b5db47|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|504c37261562da55|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|548693dd960adab7|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|5bddf535bb88630f|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|6a15731b07a61d11|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|6c92be2be26088f1|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|7d774c08fb4dcb0f|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|8251182e6886892b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|90b40b32f3c80e67|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|9ace742a8a2da8bc|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|9fcd4fc43c360e3d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|9fcd4fc43c360e3d|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|a08a942192c7b26d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|a08a942192c7b26d|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|a5f04ce82f83934d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|a78f93471475f723|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|aa10ea56d0541673|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|b6e6736b30bb084d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|c25a011cb37ad59a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|c25a011cb37ad59a|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|cbdfc265cbf0e547|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|d240926414b31170|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|d3867054f98d9d1c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|d903e2cbb0769cb9|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|ddd05aeaedc750e9|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|e8f1e829db2eac56|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|ecde4dbdf9e29c30|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|ecde4dbdf9e29c30|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|ecde4dbdf9e29c30|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CloseHandle|f8184c1242de1706|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CoTaskMemFree|16ef00703efb18f6|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CoTaskMemFree|16ef00703efb18f6|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CommandLineToArgvW|109409c0aaa97db7|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|ConnectNamedPipe|4fb329e0499ad2d8|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateEventW|029ac72f34ad8d85|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateFileW|27754a2524a1a0e2|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateFileW|4bd1558ed1245b2e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateFileW|7325fcbbdef5935b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateFileW|8a8f14e77ea9aed2|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateFileW|8d049e7dbcd8fe30|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateFileW|97174c09de3c914f|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateFileW|9b90f504ae769921|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateFileW|b464b9d758e6fb98|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateFileW|b464b9d758e6fb98|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateJobObjectW|6486f9f3e29000a4|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateJobObjectW|9370335f979a7a53|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateJobObjectW|a66370923291a747|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateNamedPipeW|5812d09af06df94a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreatePipe|3afe601c52316141|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreatePipe|ad1635c9d0296bf7|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreatePipe|db310af0a09784ca|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateProcessW|2108dba4624bb85f|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateProcessW|24e0a8be633d695f|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CreateProcessW|24e0a8be633d695f|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CryptAcquireContextW|314bb1444209ae58|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CryptAcquireContextW|96cdfa095976a766|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CryptCreateHash|3808bae20b5ae882|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CryptDestroyHash|c4071bd2abf1285a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CryptGenRandom|2a550484cb17e738|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CryptGetHashParam|a0d3c763c245e8b0|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CryptHashData|d32e501f65a5eefa|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CryptReleaseContext|08afe7426ad1964e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|CryptReleaseContext|346a4fabcef4c093|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DeleteProcThreadAttributeList|5af493dbebacfe91|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DuplicateHandle|2b5257bdce691171|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DuplicateHandle|358d58b261ab4fa9|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DuplicateHandle|3c9512018cedba6a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DuplicateHandle|3cd9042a76b8496e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DuplicateHandle|4a8010cc1d0e0242|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DuplicateHandle|554d2fc7f7a6e46d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DuplicateHandle|75af7eea6b18f83c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DuplicateHandle|d023fffe91e15137|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DuplicateHandle|d023fffe91e15137|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DuplicateHandle|e4061afacb1a1c3d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|DuplicateHandle|f2bccb9b4665b421|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|EqualSid|b9bbe4b352c2dd00|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|FlushFileBuffers|d49eb3fdc1337a73|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCommandLineW|109409c0aaa97db7|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCommandLineW|baf08b022d91e96c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentDirectoryW|b6b6e57762f516f1|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcessId|7155de8444ea1b1f|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|033e967a698277fe|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|0483d85554588b01|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|06c8f5a19a1b1c12|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|2b5257bdce691171|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|2b5257bdce691171|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|3a356f961bc9c04a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|60fb03bdf78baaa0|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|75af7eea6b18f83c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|abf5ae9070c72502|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|de276a431f427fb1|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|de276a431f427fb1|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|eefb8f2d8fb7ee10|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetCurrentProcess|f27ff963c5635251|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetEnvironmentVariableW|19e5a9c3d6a6e1ad|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetEnvironmentVariableW|58e6d185bf23c3e7|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetEnvironmentVariableW|e694faa5a7740fac|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetEnvironmentVariableW|fd37b2ac50bb68cb|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|1254d8b060fda0ab|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|1254d8b060fda0ab|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|25b3f606bdf6b997|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|25b3f606bdf6b997|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|25b3f606bdf6b997|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|25b3f606bdf6b997|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|25b3f606bdf6b997|5',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|25b3f606bdf6b997|6',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|25b3f606bdf6b997|7',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|75a3d7283ede8231|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|7b98bdc9ffa1b1bf|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|d524f1d77ee18252|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileInformationByHandleEx|de0664bdf04be914|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileSizeEx|b4436911c10ee462|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|13ad141789b00362|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|13ad141789b00362|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|180baec63a732f33|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|3a07561ba9a9f262|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|4b7547259be5518d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|51de82c899ad9760|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|5733ec3b174d4f4b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|87ec472c4dc06230|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|9cada58c3d7a3634|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|a85452281559059c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|adc43735146d875e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|bc7ccdeef83b4b32|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFileType|e37ca915f9677f0b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetFinalPathNameByHandleW|15b16cfcabb4629c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|025ab8574e758995|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|035f46dac73ba10c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|035f46dac73ba10c|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|035f46dac73ba10c|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|035f46dac73ba10c|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0a8193a63035c703|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0abf1d918e1583d4|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0abf1d918e1583d4|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0c77b9be4e942b1f|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0c77b9be4e942b1f|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0c77b9be4e942b1f|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0c77b9be4e942b1f|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0c77b9be4e942b1f|5',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0c77b9be4e942b1f|6',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0c77b9be4e942b1f|7',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0f63eb29639c3af4|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|0fdab409e400d411|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|102654b6e380cb9a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|1168b41117cc8141|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|1168b41117cc8141|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|1168b41117cc8141|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|2d7167f0bf2a1eb2|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|30daa2de4d3aa083|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|331074e62ae9e919|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|36121ad516c0223d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|3b50283fb9b51717|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|3de00a11909c551d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|3de00a11909c551d|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|40523cec8c138f8a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|4ef886624ffb0208|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|547d9a782ebed61e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|10',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|11',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|12',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|13',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|14',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|15',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|16',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|17',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|18',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|19',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|20',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|21',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|22',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|23',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|5',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|6',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|7',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|8',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|5dd08c5ecf57b63e|9',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|6216ea7483093757|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|65a9cf3a0de490f3|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|704fb7222b651857|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|748c50aed1973464|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|7e5fc35fb39278ca|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|7fb6746603686446|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|8225e9f769b41aa5|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|894697007db7b527|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|8dfcc62f82cb8ae7|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|8dfcc62f82cb8ae7|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|8dfcc62f82cb8ae7|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|8dfcc62f82cb8ae7|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|9bae5bb00947e03f|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|9bae5bb00947e03f|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|9bae5bb00947e03f|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|a26d1a04fdcb2db8|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|a9fb1afa247b0895|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|aa36178de899fcdb|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|ac3c4b87fe1df8b4|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|ae1dd62ff72826cc|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|b2c599d147b225da|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|b2c599d147b225da|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|bc76b55b440007ad|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|be451ef45afc28f3|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|be451ef45afc28f3|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|be451ef45afc28f3|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|be451ef45afc28f3|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|be451ef45afc28f3|5',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|be451ef45afc28f3|6',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|be451ef45afc28f3|7',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|c5ec4ba866dabe7b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|c5ec4ba866dabe7b|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|c5ec4ba866dabe7b|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|ce338fb63c2784e2|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d06cb039c2b8bcd7|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d6183be8f4af725e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d6183be8f4af725e|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d6183be8f4af725e|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|10',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|11',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|12',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|13',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|14',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|15',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|16',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|17',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|5',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|6',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|7',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|8',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|d799bb079b9a857d|9',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|dc314c60c519c00e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|e0b83ee1d8e31ee1|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|e34823463e81fd55|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|e8f0b7b2eb97d230|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|e99892c7cbb36b60|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|eeb64d23c415c6de|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|f113d65aab562ffb|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|f113d65aab562ffb|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|f113d65aab562ffb|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|f113d65aab562ffb|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|f113d65aab562ffb|5',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|f113d65aab562ffb|6',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|f113d65aab562ffb|7',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|f113d65aab562ffb|8',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLastError|f3445102531e8c64|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLengthSid|0a6967723755328d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetLengthSid|c21e6e8a764aa5a8|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetModuleFileNameW|4ad2b3a6c736fc7d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetNamedPipeServerProcessId|d18e8edf77fe2aaa|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetNamedPipeServerProcessId|d18e8edf77fe2aaa|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetOverlappedResult|cd2762c2dcd14149|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetOverlappedResult|cd2762c2dcd14149|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetProcessId|346908dcd439a7d8|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetProcessId|9c189a52d1270c45|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetProcessId|9c189a52d1270c45|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetProcessId|b69103f924a1ae8b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetProcessId|c4248772e9ccbda8|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetProcessId|f0462119acfcb2fd|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetProcessId|f18ae72961ee44a3|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetProcessTimes|173322853dce917c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetProcessTimes|59d02503c5ac1fec|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetStdHandle|71eb5d608ee47f17|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetStdHandle|d28547d42e66fde5|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetStdHandle|e3df69b6c452fce0|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|0c9f6e7065da4fe9|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|0c9f6e7065da4fe9|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|0c9f6e7065da4fe9|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|0cc4c1d361e31e3a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|0cc4c1d361e31e3a|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|0cc4c1d361e31e3a|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|1c935603595669c7|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|1c935603595669c7|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|1e35047c40a5bde4|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|1e35047c40a5bde4|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|6e7e35fb10d71574|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|6e7e35fb10d71574|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|ba2e088843a1776e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTickCount64|d4ff1d206dcc3fc1|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTokenInformation|28c9fb8d74071cc5|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTokenInformation|600aacf7614807f4|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTokenInformation|743fd39db9e46277|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|GetTokenInformation|839d4f29f99c1f80|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|InitializeAcl|180d358397447e6b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|InitializeAcl|a6e50e9a45b62436|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|InitializeProcThreadAttributeList|125826e7d0435143|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|InitializeProcThreadAttributeList|ad3fc43df3809375|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|InitializeProcThreadAttributeList|ad3fc43df3809375|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|InitializeProcThreadAttributeList|ad3fc43df3809375|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|InitializeSecurityDescriptor|ae6cb01e4616fff8|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|InitializeSecurityDescriptor|e501d9cf2371e882|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|IsProcessInJob|3367698c644ce153|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|IsProcessInJob|36687eb755d75537|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|IsProcessInJob|3a356f961bc9c04a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|IsProcessInJob|abf5ae9070c72502|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|IsProcessInJob|b87f143aa6df5ebc|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|IsProcessInJob|fa9193101e639b27|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|IsValidSid|57e2dc664ca15f87|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|IsValidSid|c7c99d18bc46fb15|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|IsValidSid|cbce5e5abcb3184a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|IsValidSid|f909dcc8ddf01588|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|LocalFree|e8ca5dec00070824|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|MessageBoxW|767ba0a566e946d8|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|MultiByteToWideChar|381de0015ed516cf|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|MultiByteToWideChar|50ec9fee9bea75f6|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|MultiByteToWideChar|7afd2c12aa1cd3c6|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|MultiByteToWideChar|82a634782f561c4b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|OpenJobObjectW|19efedc591ebd53c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|OpenProcessToken|18cd8e84c8aa5b14|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|OpenProcessToken|60fb03bdf78baaa0|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|OpenProcess|a876150ebd79d124|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|OpenProcess|a876150ebd79d124|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|OpenProcess|d6ec58abf253fe2d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|OpenProcess|d6ec58abf253fe2d|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|PeekNamedPipe|26b799e8c3958e82|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|PeekNamedPipe|8eb98e530e378288|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|PeekNamedPipe|8eb98e530e378288|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|PeekNamedPipe|8eb98e530e378288|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|QueryFullProcessImageNameW|eedf2eeec1ab39c2|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|QueryInformationJobObject|134496d5b55e28d5|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|QueryInformationJobObject|134496d5b55e28d5|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|QueryInformationJobObject|134496d5b55e28d5|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|QueryInformationJobObject|134496d5b55e28d5|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|QueryInformationJobObject|134496d5b55e28d5|5',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|QueryInformationJobObject|1b47c32ce5a2ace2|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|QueryInformationJobObject|1b47c32ce5a2ace2|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|QueryInformationJobObject|970db5c435454d7f|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|ReadFile|3565543304461679|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|ReadFile|3565543304461679|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|ReadFile|807d610f2f692c5c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|ReadFile|807d610f2f692c5c|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|ReadFile|81c583161f1c08e1|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|ReadFile|ea7ee11c82c50b70|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|ResumeThread|30a9587fde3fc9cf|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|ResumeThread|e8d734d4a14adc78|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SHGetKnownFolderPath|b491ce5babb6eef6|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SHGetKnownFolderPath|b491ce5babb6eef6|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetFilePointerEx|d350d4b9cd126b82|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetHandleInformation|15942ab747cddc80|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetHandleInformation|16425ac85d136acc|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetHandleInformation|59fc6e00a5c7f9fa|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetHandleInformation|69aeec6328e9fe18|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetHandleInformation|7bac1be47eb94e5c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetHandleInformation|7f7af7ab8b010dbe|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetHandleInformation|8390a60b50f689ef|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetHandleInformation|902caef304d3e51e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetHandleInformation|cf8f2ffefd6addff|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetHandleInformation|d325471aaea8d499|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetInformationJobObject|16114d22919b215c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetInformationJobObject|9d6359845702015b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetInformationJobObject|af715ec9ec19f2aa|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetInformationJobObject|b1e6d60ece589454|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetInformationJobObject|b1e6d60ece589454|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetInformationJobObject|df6519b0ae511a89|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetLastError|b0980f149dc31d86|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetLastError|b0980f149dc31d86|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetLastError|b0980f149dc31d86|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetLastError|b0980f149dc31d86|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetLastError|b0980f149dc31d86|5',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetLastError|b0980f149dc31d86|6',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetLastError|b0980f149dc31d86|7',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetSecurityDescriptorDacl|142f8aad9233d92e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|SetSecurityDescriptorDacl|cf579cbea62acdcf|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|Sleep|3fb3e2ee0cb7ed01|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|Sleep|3fb3e2ee0cb7ed01|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|Sleep|3fb3e2ee0cb7ed01|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|Sleep|882e99c4c258b9ed|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|Sleep|9995c212f810466f|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|Sleep|9995c212f810466f|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|TerminateJobObject|3c64d66a8d755412|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|TerminateJobObject|a45f1c22fe67e516|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|TerminateJobObject|de00782ae6de2187|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|TerminateProcess|9cc17b9bb7e6549d|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|TerminateProcess|9cc17b9bb7e6549d|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|TerminateProcess|9cc17b9bb7e6549d|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|TerminateProcess|9cc17b9bb7e6549d|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|TerminateProcess|9cc17b9bb7e6549d|5',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|TerminateProcess|9cc17b9bb7e6549d|6',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|TerminateProcess|a9c69437637765ea|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|UpdateProcThreadAttribute|310070d7a2bf41c7|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|UpdateProcThreadAttribute|310070d7a2bf41c7|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|UpdateProcThreadAttribute|310070d7a2bf41c7|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|UpdateProcThreadAttribute|310070d7a2bf41c7|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|UpdateProcThreadAttribute|c852ee94fed6a331|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WTHelperGetProvSignerFromChain|2dbccab2804949a8|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WTHelperProvDataFromStateData|a0ef1f12c96fa01b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|05223bffe4808e3b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|05223bffe4808e3b|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|212092482c4dd3fb|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|212092482c4dd3fb|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|2428ffae9c013a98|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|26469cf121033297|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|30be1fc456008a7c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|30be1fc456008a7c|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|30be1fc456008a7c|3',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|30be1fc456008a7c|4',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|30be1fc456008a7c|5',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|30be1fc456008a7c|6',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|402be7a6a2fe3571|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|5944d91b6fe53e23|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|76fb88501ae687a4|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|859dafe782e6f8c9|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|873a314b73d78dbb|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|873a314b73d78dbb|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|87ce60d2a83e438a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|8d2bf1d6f9c4e0d8|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|97122ec77cdb6e20|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|9d57f66b6e076422|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|9e270bfd8c9413fb|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|9fe9b0d174d68c8e|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|a0e16a1e25118ab8|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|b2c4a20669add33b|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|b2c4a20669add33b|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|b64a13ae37666cb7|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|b86024ce0cc4daaa|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|c2b2179ba8597969|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|c2b2179ba8597969|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|c401f83c67fb755c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|c694fb3259bf93f2|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|ddb3fe497a89c9ff|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|e1519ba0b1756962|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|eb8efece2287e7bd|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|eb8efece2287e7bd|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WaitForSingleObject|f1229c73b820550c|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WideCharToMultiByte|3c88cbaff44a0119|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WideCharToMultiByte|c3e229c3fbc4c8ad|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WinVerifyTrust|2e600284dab9db3a|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WinVerifyTrust|8c53ca2371b0b364|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WriteFile|01dac41a0dcba129|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WriteFile|1a48191602586ffe|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WriteFile|a1cb3f1b25febc49|1',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WriteFile|a1cb3f1b25febc49|2',
    'src/bootstrap/WindowsBootstrapRuntimeWin32.cpp|native-symbol|WriteFile|da83242839cdee66|1',
})

APPROVED_UPDATE_APPLICATION_PLATFORM_ADAPTER_FINGERPRINTS = frozenset({
    'src/update/ArisUpdateComposition.cpp|filesystem-io|std::filesystem::create_directories|10b7b26444095dc2|1',
    'src/update/ArisUpdateComposition.cpp|filesystem-io|std::filesystem::create_directories|3d54e5bffcfa4e62|1',
    'src/update/ArisUpdateComposition.cpp|filesystem-io|std::filesystem::exists|c4f9b285c676d268|1',
    'src/update/ArisUpdateComposition.cpp|filesystem-io|std::filesystem::is_directory|0cb96061a35e847b|1',
    'src/update/ArisUpdateComposition.cpp|filesystem-io|std::filesystem::is_directory|e2cd66856d3c0e5a|1',
    'src/update/ArisUpdateComposition.cpp|filesystem-io|std::filesystem::recursive_directory_iterator|72752a6f661fc1b7|1',
    'src/update/ArisUpdateComposition.cpp|filesystem-io|std::filesystem::recursive_directory_iterator|ffa0951fc7e7320a|1',
    'src/update/ArisUpdateComposition.cpp|native-include|Windows.h|3d20652c81953438|1',
    'src/update/ArisUpdateComposition.cpp|native-include|bcrypt.h|ec818221406cb598|1',
    'src/update/ArisUpdateComposition.cpp|native-include|fileapi.h|7ea06f1854c8136c|1',
    'src/update/ArisUpdateComposition.cpp|native-include|shlobj.h|2d3e62b5423f2cb0|1',
    'src/update/ArisUpdateComposition.cpp|native-include|wintrust.h|2a804c471cf6c52e|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|BCryptGenRandom|165fffb62e024ad4|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|070e943497dae143|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|2389d9b23be18a48|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|24719bfdbeb8bb6a|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|24719bfdbeb8bb6a|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|2746a0504d7175ca|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|2746a0504d7175ca|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|2746a0504d7175ca|3',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|29058c045d3c7390|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|29058c045d3c7390|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|2ef9ade4bf3b5a4a|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|34c58f48092007a9|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|3f4efe6d6cf8e4ee|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|48e1c1a82e04c12d|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|48e1c1a82e04c12d|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|4e6a2d568e3a7af4|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|6328d351c7a75dd0|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|66a11306c50ca72f|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|6773e687b0b328e0|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|80df7621a7766bf6|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|80df7621a7766bf6|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|938cc8576c56070a|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|ba03ca6203d28c46|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|ba03ca6203d28c46|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|ba03ca6203d28c46|3',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|c25a011cb37ad59a|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|c25a011cb37ad59a|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|c6e49b8fdef748a6|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|c6e49b8fdef748a6|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|c6e49b8fdef748a6|3',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|c8dae9f595efbe6c|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|cd0946fa099a1fa3|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|d6c8e54edf0c5315|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|d6c8e54edf0c5315|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|d6c8e54edf0c5315|3',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|d6c8e54edf0c5315|4',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|d6c8e54edf0c5315|5',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|d6c8e54edf0c5315|6',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|d6c8e54edf0c5315|7',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|d6c8e54edf0c5315|8',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|d6c8e54edf0c5315|9',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|e885e6ce1b0e5631|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|e885e6ce1b0e5631|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|eaf9391bf5582542|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|ebb975c6e7e3d002|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|f43dea07197d5d7c|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|f70527ab9e713348|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|f70527ab9e713348|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|fd917ea3fb2a5265|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|fd917ea3fb2a5265|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CloseHandle|fd917ea3fb2a5265|3',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CoTaskMemFree|d8026b138e7a6555|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CoTaskMemFree|d8026b138e7a6555|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|ConnectNamedPipe|1f62b2ce79c042d8|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CreateFileW|263393dfe0c16d05|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CreateFileW|6c12c6c3be0511bf|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CreateFileW|9e08739e46afe0fb|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CreateFileW|9e08739e46afe0fb|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CreateJobObjectW|378c3cf407260388|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CreateNamedPipeW|687225f5381391ef|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CreateNamedPipeW|b9013d7de2be2331|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CreatePipe|08d16ba24fa2e283|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CreateProcessW|2108dba4624bb85f|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|CreateProcessW|2108dba4624bb85f|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|DeleteProcThreadAttributeList|0cf5bbd6a12dbfeb|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|DeleteProcThreadAttributeList|0cf5bbd6a12dbfeb|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|DeleteProcThreadAttributeList|0cf5bbd6a12dbfeb|3',
    'src/update/ArisUpdateComposition.cpp|native-symbol|DeleteProcThreadAttributeList|0cf5bbd6a12dbfeb|4',
    'src/update/ArisUpdateComposition.cpp|native-symbol|DuplicateHandle|3bcc941bf76581aa|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetCurrentProcessId|11fccc8d36f2ca8b|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetCurrentProcessId|187c03c7a38e9bf1|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetCurrentProcessId|343e29038a2e2c42|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetCurrentProcessId|980d80218498bc05|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetCurrentProcessId|980d80218498bc05|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetCurrentProcess|1ab5e2869ffeefdd|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetCurrentProcess|3bcc941bf76581aa|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetCurrentProcess|3bcc941bf76581aa|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetCurrentProcess|556e06a43e4a3c25|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetCurrentProcess|cb8c44f79df50e93|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetCurrentProcess|fa584549a2ecc864|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetFileAttributesW|c0b2220988827182|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetFileInformationByHandleEx|25b3f606bdf6b997|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetFileInformationByHandleEx|25b3f606bdf6b997|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetFileInformationByHandleEx|25b3f606bdf6b997|3',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetFileInformationByHandleEx|5d88dc2068b885ad|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetFileType|f31878dce42f80c0|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetFinalPathNameByHandleW|119ad8fc07daeadd|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetFinalPathNameByHandleW|119ad8fc07daeadd|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetFinalPathNameByHandleW|efa5899c88877ed6|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetFinalPathNameByHandleW|efa5899c88877ed6|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetLastError|bfb1ff59e5b1167e|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetLastError|bfb1ff59e5b1167e|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetLastError|f3939ca6ea5f41e8|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetModuleFileNameW|132ef2ac56fffd05|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetProcessId|a85510e038c9ac80|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetProcessTimes|556e06a43e4a3c25|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetProcessTimes|a8313c1aae219da6|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetStdHandle|5da364cf857eff6c|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetStdHandle|7ba336161fe15d19|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetTickCount64|1c935603595669c7|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetTickCount64|62aa13c00c91d91d|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetTickCount64|d97786aca9257b30|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetTickCount64|d97786aca9257b30|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetTokenInformation|4a378e8fa8b7e7ea|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|GetTokenInformation|50442f89ac0671e4|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|InitializeProcThreadAttributeList|64477bb1f856c039|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|InitializeProcThreadAttributeList|ad3fc43df3809375|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|InitializeProcThreadAttributeList|ad3fc43df3809375|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|InitializeProcThreadAttributeList|c98bda0e98010814|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|LocalFree|6e7202607919210c|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|OpenProcessToken|cb8c44f79df50e93|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|PeekNamedPipe|8eb98e530e378288|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|QueryFullProcessImageNameW|20276dd382dea82e|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|QueryFullProcessImageNameW|3e6379fee4ae58b0|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|ReadFile|9bab16f516c1e0cc|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|ResumeThread|5343720e96c04351|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|SHGetKnownFolderPath|53392190dcd9c321|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|SHGetKnownFolderPath|6c84f0db27c067fd|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|SetHandleInformation|05d24e7d239541e8|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|SetHandleInformation|26bad865afb9c6a1|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|SetHandleInformation|4cc7a04518f5e925|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|SetHandleInformation|6d5d84e4081db0de|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|SetHandleInformation|b3862926ff02e729|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|SetHandleInformation|b8a84d83db23c2c6|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|SetHandleInformation|ceb11ce4fe63e869|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|SetInformationJobObject|5b77beeb24e45c61|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|Sleep|3fb3e2ee0cb7ed01|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|TerminateProcess|0d1aa3eed03e0d1c|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|TerminateProcess|25aaa43835d266c0|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|UpdateProcThreadAttribute|c852ee94fed6a331|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|UpdateProcThreadAttribute|c852ee94fed6a331|2',
    'src/update/ArisUpdateComposition.cpp|native-symbol|UpdateProcThreadAttribute|c852ee94fed6a331|3',
    'src/update/ArisUpdateComposition.cpp|native-symbol|WaitForSingleObject|45a78d48e832f19d|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|WriteFile|71f036ef76791a84|1',
    'src/update/ArisUpdateComposition.cpp|native-symbol|WriteFile|e7acc02095f4f70a|1',
})

APPROVED_UPDATE_CHECK_HTTPS_PLATFORM_ADAPTER_FINGERPRINTS = frozenset({
    'src/update/UpdateCheckHttpsTransport.cpp|native-include|windows.h|d395cd89463e8f58|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-include|wininet.h|070ddd9da01e1ce0|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|GetLastError|26b6730f508a5d12|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|GetLastError|4838af9c562129d0|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|InternetCloseHandle|05424715ac2fd0ed|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|InternetCloseHandle|05424715ac2fd0ed|2',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|InternetCloseHandle|3497708c7aa6f77b|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|InternetCloseHandle|c4b6ca53384d9cfe|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|InternetCloseHandle|c54bcde3f1926224|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|InternetCloseHandle|c54bcde3f1926224|2',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|InternetOpenW|cde1d86e203e61b4|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|InternetReadFile|d214e9d4d8b349f5|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|MultiByteToWideChar|2fbd27e43cb69f5e|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|MultiByteToWideChar|50ec9fee9bea75f6|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|WideCharToMultiByte|0e82f3503d1b3631|1',
    'src/update/UpdateCheckHttpsTransport.cpp|native-symbol|WideCharToMultiByte|c3e229c3fbc4c8ad|1',
})

# The updater Win32 implementations moved to src/platform in this release
# branch. Keep the historical fingerprints available for report archaeology,
# but do not classify the retired src/update paths as live audited profiles.
APPROVED_UPDATE_APPLICATION_PLATFORM_ADAPTER_FINGERPRINTS = frozenset()
APPROVED_UPDATE_CHECK_HTTPS_PLATFORM_ADAPTER_FINGERPRINTS = frozenset()

APPROVED_BOOTSTRAP_EXIT_PLATFORM_ADAPTER_FINGERPRINTS = frozenset({
    "src/bootstrap/HarkenSearchBootstrapExitExplanation.cpp|native-include|windows.h|d395cd89463e8f58|1",
    "src/bootstrap/HarkenSearchBootstrapExitExplanation.cpp|native-include|knownfolders.h|d6c6ac6448757e97|1",
    "src/bootstrap/HarkenSearchBootstrapExitExplanation.cpp|native-include|shlobj.h|2d3e62b5423f2cb0|1",
    "src/bootstrap/HarkenSearchBootstrapExitExplanation.cpp|native-symbol|GetModuleFileNameW|94ac65cee4dc3d7d|1",
    "src/bootstrap/HarkenSearchBootstrapExitExplanation.cpp|native-symbol|SHGetKnownFolderPath|6be44be6482d7577|1",
    "src/bootstrap/HarkenSearchBootstrapExitExplanation.cpp|native-symbol|CoTaskMemFree|201d0bb7065d19b7|1",
})

# Exact Windows updater UI calls independently reviewed on 2026-10-29.
BOOTSTRAP_PROGRESS_PLATFORM_ADAPTER_EVIDENCE = "docs/architecture/bootstrap-progress-native-audit.md"
BOOTSTRAP_PROGRESS_PLATFORM_ADAPTER_FINGERPRINTS = frozenset({
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-include|windows.h|d395cd89463e8f58|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-include|objbase.h|cd73b8c775fbde36|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|MultiByteToWideChar|c68e9c5432d1c4ec|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|MultiByteToWideChar|42128e961dd0e76a|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|GetCurrentThreadId|14c26801c82dd79d|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|CreateEventW|d8a16b06cf30572d|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|CloseHandle|909ef616472bca95|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|GetTickCount64|ba7734b086741e0f|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|WaitForSingleObject|e9a68ec0c20b7356|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|GetTickCount64|73bbbcef2e3ea6f9|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|GetTickCount64|642672de4b128013|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|GetEnvironmentVariableW|d00269b1db891625|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|GetTickCount64|c01d21e1596790f8|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|GetTickCount64|014abf309af03e1c|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|filesystem-io|std::ofstream|37fdc8f35b485421|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|WideCharToMultiByte|2a2281027e482bbf|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|WideCharToMultiByte|9f5ee37047b0a62f|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|filesystem-io|std::ofstream|87c4b24b2d50f33b|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|ShowWindow|cf4584c070c6d23f|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|ShowWindow|f3cde9e55e38592d|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|GetTickCount64|aa819b70ba039371|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|GetTickCount64|dd99fb19e0820a08|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|ShowWindow|bf1a6068d11c41b6|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|SetEvent|488bfd035a60446b|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|CoInitializeEx|581a963ecc451f71|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|ShowWindow|d05e31e981016cc7|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|ShowWindow|1598f1290d28d994|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|SetEvent|7df14c6d067e2178|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|SetEvent|7df14c6d067e2178|2',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|CoUninitialize|a54dd9fe1ff88231|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|GetTickCount64|b451c7a15c321416|1',
    'src/bootstrap/WindowsBootstrapProgress.cpp|native-symbol|GetTickCount64|ef776053567162bc|1',
    'src/bootstrap/WindowsBootstrapWebView.cpp|native-include|WebView2.h|b2137ebe9b764182|1',
    'src/bootstrap/WindowsBootstrapWebView.cpp|native-include|wrl.h|9a6f80392d3201fd|1',
    'src/bootstrap/WindowsBootstrapWebView.cpp|native-include|shlobj.h|2d3e62b5423f2cb0|1',
    'src/bootstrap/WindowsBootstrapWebView.cpp|native-include|shlwapi.h|71cee191199b1510|1',
    'src/bootstrap/WindowsBootstrapWebView.cpp|native-symbol|MultiByteToWideChar|f1a7fe8b1551ed93|1',
    'src/bootstrap/WindowsBootstrapWebView.cpp|native-symbol|MultiByteToWideChar|55b1ab4bfecb6a6a|1',
    'src/bootstrap/WindowsBootstrapWebView.cpp|native-symbol|IsWindow|10c35cbd47c1d695|1',
    'src/bootstrap/WindowsBootstrapWebView.cpp|native-symbol|SHGetKnownFolderPath|42428b53b56e47c0|1',
    'src/bootstrap/WindowsBootstrapWebView.cpp|native-symbol|CoTaskMemFree|021f0cee7174197a|1',
    'src/bootstrap/WindowsBootstrapWebView.cpp|native-symbol|GetCurrentProcessId|f006b35fa5f0d07d|1',
    'src/bootstrap/WindowsBootstrapWebView.cpp|native-symbol|CoTaskMemFree|2f6f35dbc21206c2|1',
    'src/bootstrap/WindowsBootstrapWebView.h|native-include|windows.h|d395cd89463e8f58|1',
})

PLATFORM_ADAPTER_PROFILES = {
    'src/bootstrap/WindowsBootstrapProgress.cpp': (
        "BOOTSTRAP-PROGRESS", BOOTSTRAP_PROGRESS_PLATFORM_ADAPTER_EVIDENCE,
        frozenset({"native-include", "native-symbol", "filesystem-io"}),
        frozenset(fingerprint for fingerprint in BOOTSTRAP_PROGRESS_PLATFORM_ADAPTER_FINGERPRINTS
                  if fingerprint.startswith('src/bootstrap/WindowsBootstrapProgress.cpp|')),
    ),
    'src/bootstrap/WindowsBootstrapWebView.cpp': (
        "BOOTSTRAP-PROGRESS", BOOTSTRAP_PROGRESS_PLATFORM_ADAPTER_EVIDENCE,
        frozenset({"native-include", "native-symbol", "filesystem-io"}),
        frozenset(fingerprint for fingerprint in BOOTSTRAP_PROGRESS_PLATFORM_ADAPTER_FINGERPRINTS
                  if fingerprint.startswith('src/bootstrap/WindowsBootstrapWebView.cpp|')),
    ),
    'src/bootstrap/WindowsBootstrapWebView.h': (
        "BOOTSTRAP-PROGRESS", BOOTSTRAP_PROGRESS_PLATFORM_ADAPTER_EVIDENCE,
        frozenset({"native-include", "native-symbol", "filesystem-io"}),
        frozenset(fingerprint for fingerprint in BOOTSTRAP_PROGRESS_PLATFORM_ADAPTER_FINGERPRINTS
                  if fingerprint.startswith('src/bootstrap/WindowsBootstrapWebView.h|')),
    ),
    PLATFORM_ADAPTER_PATH: (
        PLATFORM_ADAPTER_OWNER,
        PLATFORM_ADAPTER_EVIDENCE,
        PLATFORM_ADAPTER_RULES,
        APPROVED_PLATFORM_ADAPTER_FINGERPRINTS,
    ),
    RUNTIME_PLATFORM_ADAPTER_PATH: (
        RUNTIME_PLATFORM_ADAPTER_OWNER,
        RUNTIME_PLATFORM_ADAPTER_EVIDENCE,
        PLATFORM_ADAPTER_RULES,
        APPROVED_RUNTIME_PLATFORM_ADAPTER_FINGERPRINTS,
    ),
    BOOTSTRAP_EXIT_PLATFORM_ADAPTER_PATH: (
        BOOTSTRAP_EXIT_PLATFORM_ADAPTER_OWNER,
        BOOTSTRAP_EXIT_PLATFORM_ADAPTER_EVIDENCE,
        PLATFORM_ADAPTER_RULES,
        APPROVED_BOOTSTRAP_EXIT_PLATFORM_ADAPTER_FINGERPRINTS,
    ),
    UPDATE_APPLICATION_PLATFORM_ADAPTER_PATH: (
        UPDATE_APPLICATION_PLATFORM_ADAPTER_OWNER,
        UPDATE_APPLICATION_PLATFORM_ADAPTER_EVIDENCE,
        UPDATE_APPLICATION_PLATFORM_ADAPTER_RULES,
        APPROVED_UPDATE_APPLICATION_PLATFORM_ADAPTER_FINGERPRINTS,
    ),
    UPDATE_CHECK_HTTPS_PLATFORM_ADAPTER_PATH: (
        UPDATE_CHECK_HTTPS_PLATFORM_ADAPTER_OWNER,
        UPDATE_CHECK_HTTPS_PLATFORM_ADAPTER_EVIDENCE,
        PLATFORM_ADAPTER_RULES,
        APPROVED_UPDATE_CHECK_HTTPS_PLATFORM_ADAPTER_FINGERPRINTS,
    ),
}

NATIVE_INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"](?P<token>(?:bcrypt|combaseapi|commdlg|coreservices/coreservices|dbghelp|dirent|fileapi|fcntl|knownfolders|mfapi|mfidl|mfreadwrite|mmsystem|objbase|ole2|propvarutil|psapi|shellapi|shlobj|shlwapi|shobjidl|softpub|tlhelp32|unistd|webview2|winbase|wincodec|wincrypt|windows|wininet|wintrust|winsock2|ws2tcpip)(?:\.h)?|wrl(?:/client)?\.h|sys/[^>"]+)[>"]',
    re.IGNORECASE,
)
POSIX_INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"](?:unistd|dirent|fcntl)(?:\.h)?[>"]|^\s*#\s*include\s*[<"]sys/[^>"]+[>"]',
    re.IGNORECASE | re.MULTILINE,
)
FILESYSTEM_IO = re.compile(
    r"\b(?P<token>(?:std::filesystem|fs)::(?:exists|remove|remove_all|rename|copy|copy_file|create_directory|create_directories|directory_iterator|recursive_directory_iterator|file_size|last_write_time|status|space|is_directory|is_regular_file|is_symlink|equivalent|canonical|weakly_canonical|absolute|relative|temp_directory_path|current_path|permissions|resize_file|symlink_status))\b"
    r"|\b(?P<stream>std::(?:ifstream|ofstream|fstream|filebuf|basic_ifstream|basic_ofstream|basic_fstream))\b"
    r"|\b(?P<cfile>fopen|_wfopen|freopen|_wfreopen)\s*\("
)
AUDIO_IO_TYPE = re.compile(r"\b(?P<token>ma_[A-Za-z0-9_]+)\b")
NATIVE_SYMBOL = re.compile(
    r"\b(?P<token>_wcsicmp|AddAccessAllowedAce|AssignProcessToJobObject|CertGetCertificateContextProperty|CommandLineToArgvW|CreateFileA|CreateFileW|CloseHandle|CreateJobObjectW|CreatePipe|CryptAcquireContextW|CryptCreateHash|CryptDestroyHash|CryptGenRandom|CryptGetHashParam|CryptHashData|CryptReleaseContext|DeleteProcThreadAttributeList|DuplicateHandle|EqualSid|FindFirstFileA|FindFirstFileW|FindNextFileA|FindNextFileW|FindClose|FlushFileBuffers|GetCommandLineW|GetCurrentDirectoryW|GetEnvironmentVariableW|GetFileAttributesA|GetFileAttributesW|GetFileInformationByHandleEx|GetFileSizeEx|GetFileType|GetFinalPathNameByHandleW|GetLengthSid|GetModuleFileNameA|GetModuleFileNameW|GetNamedPipeServerProcessId|GetOverlappedResult|GetProcessId|GetProcessTimes|GetStdHandle|GetTempPathA|GetTempPathW|GetFullPathNameA|GetFullPathNameW|GetTokenInformation|InitializeAcl|InitializeProcThreadAttributeList|InitializeSecurityDescriptor|IsProcessInJob|IsValidSid|OpenJobObjectW|OpenProcessToken|PeekNamedPipe|QueryFullImageNameW|QueryFullProcessImageNameW|QueryInformationJobObject|ReadFile|RegOpenKeyExA|RegOpenKeyExW|RegQueryValueExA|RegQueryValueExW|RegCreateKeyExA|RegCreateKeyExW|RegSetValueExA|RegSetValueExW|RegCloseKey|CoInitializeEx|CoUninitialize|CoCreateInstance|CoTaskMemFree|CreateProcessA|CreateProcessW|ShellExecuteA|ShellExecuteW|SHGetFolderPathA|SHGetFolderPathW|SHGetKnownFolderPath|CreateNamedPipeA|CreateNamedPipeW|ConnectNamedPipe|CreateEventA|CreateEventW|SetEvent|ResetEvent|WaitForSingleObject|WaitForMultipleObjects|ReadDirectoryChangesW|CancelIoEx|CreateIoCompletionPort|GetQueuedCompletionStatus|PostQueuedCompletionStatus|ResumeThread|SetFilePointerEx|SetHandleInformation|SetInformationJobObject|SetLastError|SetSecurityDescriptorDacl|SetThreadPriority|Sleep|GetTickCount|GetTickCount64|QueryPerformanceCounter|QueryPerformanceFrequency|GetLastError|FormatMessageA|FormatMessageW|LocalFree|OutputDebugStringA|OutputDebugStringW|MessageBoxA|MessageBoxW|WideCharToMultiByte|MultiByteToWideChar|GetCurrentProcess|GetCurrentProcessId|GetCurrentThreadId|OpenProcess|TerminateJobObject|TerminateProcess|EnumWindows|IsWindow|IsWindowVisible|ShowWindow|SetForegroundWindow|GetWindowThreadProcessId|InternetOpenA|InternetOpenW|InternetConnectA|HttpOpenRequestA|HttpOpenRequestW|HttpSendRequestA|InternetReadFile|InternetCloseHandle|BCryptOpenAlgorithmProvider|BCryptGenRandom|BCryptCloseAlgorithmProvider|BCryptCreateHash|BCryptHashData|BCryptFinishHash|BCryptDestroyHash|BCryptGetProperty|UpdateProcThreadAttribute|WTHelperGetProvSignerFromChain|WTHelperProvDataFromStateData|WinVerifyTrust|WriteFile)\b"
)
EXPLICIT_POSIX_OPEN = re.compile(r"(?<![\w:>])::(?P<token>open)\s*\(")
UNQUALIFIED_POSIX_OPEN = re.compile(r"(?<![\w:.>])(?P<token>open)\s*\(")
PUBLIC_NATIVE_TYPE = re.compile(
    r"\b(?P<token>HANDLE|HWND|HRESULT|IUnknown|IDispatch|IStream|ma_[A-Za-z0-9_]+)\b"
    r"|\b(?P<com>I[A-Z][A-Za-z0-9_]*)(?=\s*[*&])"
)


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    rule: str
    token: str
    source: str
    occurrence: int = 1

    @property
    def fingerprint(self) -> str:
        normalized = " ".join(self.source.strip().split())
        digest = hashlib.sha256(normalized.encode("utf-8")).hexdigest()[:16]
        return f"{self.path}|{self.rule}|{self.token}|{digest}|{self.occurrence}"


@dataclass(frozen=True)
class ExceptionClassification:
    audited_fingerprints: frozenset[str]
    unresolved_findings: list[Finding]
    errors: list[str]


def _strip_comments_and_literals(text: str) -> tuple[list[str], list[str]]:
    include_lines: list[str] = []
    code_lines: list[str] = []
    in_block_comment = False
    for raw_line in text.splitlines():
        include_chars: list[str] = []
        code_chars: list[str] = []
        quote: str | None = None
        escaped = False
        index = 0
        while index < len(raw_line):
            char = raw_line[index]
            following = raw_line[index + 1] if index + 1 < len(raw_line) else ""
            if in_block_comment:
                include_chars.append(" ")
                code_chars.append(" ")
                if char == "*" and following == "/":
                    include_chars.append(" ")
                    code_chars.append(" ")
                    in_block_comment = False
                    index += 2
                else:
                    index += 1
                continue
            if quote is not None:
                include_chars.append(char)
                code_chars.append(" ")
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = None
                index += 1
                continue
            if char == "/" and following == "/":
                padding = " " * (len(raw_line) - index)
                include_chars.append(padding)
                code_chars.append(padding)
                break
            if char == "/" and following == "*":
                include_chars.extend((" ", " "))
                code_chars.extend((" ", " "))
                in_block_comment = True
                index += 2
                continue
            if char in {'"', "'"}:
                quote = char
                escaped = False
                include_chars.append(char)
                code_chars.append(" ")
                index += 1
                continue
            include_chars.append(char)
            code_chars.append(char)
            index += 1
        include_lines.append("".join(include_chars))
        code_lines.append("".join(code_chars))
    return include_lines, code_lines


def _is_platform_implementation(path: Path, src_root: Path) -> bool:
    relative = path.relative_to(src_root)
    return len(relative.parts) > 1 and relative.parts[0].lower() == "platform" and path.suffix.lower() in {".cpp", ".mm"}


def _is_platform_header(path: Path, src_root: Path) -> bool:
    relative = path.relative_to(src_root)
    return len(relative.parts) > 1 and relative.parts[0].lower() == "platform" and path.suffix.lower() in {".h", ".hpp"}


def _finding(path: str, line: int, rule: str, token: str, source: str) -> Finding:
    return Finding(path=path, line=line, rule=rule, token=token, source=source)


def _scan_public_header(relative_path: str, include_lines: list[str], code_lines: list[str]) -> Iterable[Finding]:
    for line_number, (include_line, code_line) in enumerate(zip(include_lines, code_lines), start=1):
        include = NATIVE_INCLUDE.search(include_line)
        if include:
            yield _finding(relative_path, line_number, "public-native-type", include.group("token"), include_line)
        for match in PUBLIC_NATIVE_TYPE.finditer(code_line):
            yield _finding(relative_path, line_number, "public-native-type", match.group("token") or match.group("com"), code_line)


def _scan_business_file(relative_path: str, include_lines: list[str], code_lines: list[str]) -> Iterable[Finding]:
    has_posix_include = POSIX_INCLUDE.search("\n".join(include_lines)) is not None
    for line_number, (include_line, code_line) in enumerate(zip(include_lines, code_lines), start=1):
        include = NATIVE_INCLUDE.search(include_line)
        if include:
            yield _finding(relative_path, line_number, "native-include", include.group("token"), include_line)
        for match in FILESYSTEM_IO.finditer(code_line):
            yield _finding(
                relative_path,
                line_number,
                "filesystem-io",
                match.group("token") or match.group("stream") or match.group("cfile"),
                code_line,
            )
        for match in AUDIO_IO_TYPE.finditer(code_line):
            yield _finding(relative_path, line_number, "audio-io-type", match.group("token"), code_line)
        for match in NATIVE_SYMBOL.finditer(code_line):
            yield _finding(relative_path, line_number, "native-symbol", match.group("token"), code_line)
        explicit: set[tuple[int, int]] = set()
        for match in EXPLICIT_POSIX_OPEN.finditer(code_line):
            explicit.add(match.span("token"))
            yield _finding(relative_path, line_number, "native-symbol", match.group("token"), code_line)
        if has_posix_include:
            for match in UNQUALIFIED_POSIX_OPEN.finditer(code_line):
                if match.span("token") not in explicit:
                    yield _finding(relative_path, line_number, "native-symbol", match.group("token"), code_line)


def scan_tree(root: Path | str) -> list[Finding]:
    root_path = Path(root).resolve()
    src_root = root_path / "src"
    if not src_root.is_dir():
        return []
    results: list[Finding] = []
    occurrences: Counter[tuple[str, str, str, str]] = Counter()
    files = sorted(path for path in src_root.rglob("*") if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES)
    for path in files:
        relative_path = path.relative_to(root_path).as_posix()
        text = path.read_text(encoding="utf-8-sig", errors="replace")
        include_lines, code_lines = _strip_comments_and_literals(text)
        if _is_platform_implementation(path, src_root):
            continue
        candidates = (
            _scan_public_header(relative_path, include_lines, code_lines)
            if _is_platform_header(path, src_root)
            else _scan_business_file(relative_path, include_lines, code_lines)
        )
        for finding in candidates:
            normalized = " ".join(finding.source.strip().split())
            key = (finding.path, finding.rule, finding.token, normalized)
            occurrences[key] += 1
            results.append(replace(finding, occurrence=occurrences[key]))
    return sorted(results, key=lambda item: (item.path, item.line, item.rule, item.token, item.occurrence))


def _owner_slice(finding: Finding) -> str:
    filename = Path(finding.path).name.lower()
    stem = Path(finding.path).stem.lower()
    if "filesystemwatcher" in stem:
        return "S6-04"
    if "covermodule" in stem:
        return "S6-05A"
    if finding.rule == "audio-io-type":
        if stem in {"database", "aiengine", "waveformextractor"}:
            return "S6-05B"
        if stem in {"bpmdetector", "keydetector", "featureextractor", "centersensedatasource"}:
            return "S6-05C"
        if stem in {"audioengine", "main", "windowsmain"}:
            return "S6-06"
        return "S6-05B"
    if re.search(r"(?:protools|cubase|host|tool|bridge)", filename):
        return "S6-07B"
    if re.search(r"(?:moduleui|ui|shell)", filename):
        return "S6-07A"
    if finding.rule == "filesystem-io":
        return "S6-03"
    return "S6-02"


def build_ledger(findings: Iterable[Finding]) -> dict:
    entries = [
        {
            "fingerprint": finding.fingerprint,
            "path": finding.path,
            "line": finding.line,
            "rule": finding.rule,
            "token": finding.token,
            "occurrence": finding.occurrence,
            "owner_slice": _owner_slice(finding),
            "disposition": "migrate",
        }
        for finding in findings
    ]
    entries.sort(key=lambda entry: entry["fingerprint"])
    return {
        "schema": 1,
        "entry_tag": "refactor/stage-3-done",
        "generated_at": date.today().isoformat(),
        "findings": entries,
    }


def _resolve_evidence_file(
    root: Path,
    evidence_document: object,
    label: str,
    errors: list[str],
) -> Path | None:
    if not isinstance(evidence_document, str) or not evidence_document:
        errors.append(f"{label} must be a relative path.")
        return None
    candidate = Path(evidence_document)
    if candidate.is_absolute():
        errors.append(f"{label} must be relative: {evidence_document!r}")
        return None
    evidence_path = (root / candidate).resolve()
    try:
        evidence_path.relative_to(root.resolve())
    except ValueError:
        errors.append(f"{label} escapes repository: {evidence_document!r}")
        return None
    if not evidence_path.is_file():
        errors.append(f"{label} is missing: {evidence_document!r}")
        return None
    return evidence_path


def validate_audited_exceptions(
    findings: Iterable[Finding],
    register: dict,
    root: Path,
) -> ExceptionClassification:
    finding_list = list(findings)
    current = {finding.fingerprint: finding for finding in finding_list}
    errors: list[str] = []
    audited: set[str] = set()

    if register.get("schema") != 1:
        errors.append(f"Exception schema must be 1, got {register.get('schema')!r}.")
    evidence_path = _resolve_evidence_file(
        root,
        register.get("evidence_document"),
        "Exception evidence document",
        errors,
    )

    entries = register.get("findings", [])
    if not isinstance(entries, list):
        return ExceptionClassification(frozenset(), finding_list, errors + ["Exception field 'findings' must be a list."])

    seen: set[str] = set()
    adapter_evidence_paths: dict[str, Path | None] = {}
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            errors.append(f"Exception entry {index} must be an object.")
            continue
        fingerprint = entry.get("fingerprint")
        if not isinstance(fingerprint, str) or not fingerprint:
            errors.append(f"Exception entry {index} has no fingerprint.")
            continue
        if fingerprint in seen:
            errors.append(f"Duplicate exception fingerprint: {fingerprint}")
            continue
        seen.add(fingerprint)
        finding = current.get(fingerprint)
        if finding is None:
            errors.append(f"Stale exception entry: {fingerprint}")
            continue
        entry_valid = True
        expected = (finding.path, finding.line, finding.rule, finding.token, finding.occurrence)
        actual = (
            entry.get("path"),
            entry.get("line"),
            entry.get("rule"),
            entry.get("token"),
            entry.get("occurrence", 1),
        )
        if actual != expected:
            errors.append(f"Exception metadata mismatch: {fingerprint}")
            entry_valid = False

        disposition = entry.get("disposition")
        owner = entry.get("owner_slice")
        if disposition == "platform-entry":
            if entry.get("rule") != "native-include":
                errors.append(
                    f"Unsupported exception rule: {entry.get('rule')!r}: {fingerprint}"
                )
                entry_valid = False
            if fingerprint not in APPROVED_PLATFORM_ENTRY_FINGERPRINTS:
                errors.append(f"Unsupported platform-entry fingerprint: {fingerprint}")
                entry_valid = False
            expected_owner = _owner_slice(finding)
            if owner != expected_owner:
                errors.append(
                    f"Exception owner mismatch for {fingerprint}: "
                    f"expected {expected_owner}, got {owner}"
                )
                entry_valid = False
            if evidence_path is None or not evidence_path.is_file():
                entry_valid = False
        elif disposition == "platform-adapter":
            adapter_path = entry.get("path")
            profile = PLATFORM_ADAPTER_PROFILES.get(adapter_path)
            if profile is None:
                errors.append(
                    f"Unsupported platform-adapter path: {adapter_path!r}: "
                    f"{fingerprint}"
                )
                entry_valid = False
                approved_fingerprints = frozenset()
                expected_adapter_owner = None
                expected_adapter_evidence = None
                allowed_rules = frozenset()
            else:
                (
                    expected_adapter_owner,
                    expected_adapter_evidence,
                    allowed_rules,
                    approved_fingerprints,
                ) = profile
            if entry.get("rule") not in allowed_rules:
                errors.append(
                    f"Unsupported platform-adapter rule: {entry.get('rule')!r}: "
                    f"{fingerprint}"
                )
                entry_valid = False
            if fingerprint not in approved_fingerprints:
                errors.append(
                    f"Unsupported platform-adapter fingerprint: {fingerprint}"
                )
                entry_valid = False
            if (expected_adapter_owner is not None and
                    owner != expected_adapter_owner):
                errors.append(
                    f"Invalid platform-adapter owner for {fingerprint}: "
                    f"expected {expected_adapter_owner}, got {owner}"
                )
                entry_valid = False
            adapter_evidence = entry.get("evidence_document")
            if (expected_adapter_evidence is not None and
                    adapter_evidence != expected_adapter_evidence):
                errors.append(
                    f"Invalid platform-adapter evidence for {fingerprint}: "
                    f"expected {expected_adapter_evidence!r}, "
                    f"got {adapter_evidence!r}"
                )
                entry_valid = False
            elif expected_adapter_evidence is not None:
                if adapter_evidence not in adapter_evidence_paths:
                    adapter_evidence_paths[adapter_evidence] = _resolve_evidence_file(
                        root,
                        adapter_evidence,
                        "Platform-adapter evidence document",
                        errors,
                    )
                if adapter_evidence_paths[adapter_evidence] is None:
                    entry_valid = False
        else:
            errors.append(f"Invalid exception disposition: {disposition!r}: {fingerprint}")
            entry_valid = False

        if entry_valid:
            audited.add(fingerprint)

    unresolved = [finding for finding in finding_list if finding.fingerprint not in audited]
    return ExceptionClassification(frozenset(audited), unresolved, errors)


def validate_no_overlap(ledger: dict, audited_fingerprints: Iterable[str]) -> list[str]:
    entries = ledger.get("findings", []) if isinstance(ledger, dict) else []
    if not isinstance(entries, list):
        return []
    audited = set(audited_fingerprints)
    return [
        f"Migration ledger and audited exception overlap: {entry.get('fingerprint')}"
        for entry in entries
        if isinstance(entry, dict) and entry.get("fingerprint") in audited
    ]


def compare_ledger(findings: Iterable[Finding], ledger: dict) -> list[str]:
    errors: list[str] = []
    if ledger.get("schema") != 1:
        errors.append(f"Ledger schema must be 1, got {ledger.get('schema')!r}.")
    if ledger.get("entry_tag") != "refactor/stage-3-done":
        errors.append(f"Ledger entry tag must be 'refactor/stage-3-done', got {ledger.get('entry_tag')!r}.")
    current = {finding.fingerprint: finding for finding in findings}
    entries = ledger.get("findings", [])
    if not isinstance(entries, list):
        return ["Ledger field 'findings' must be a list."]
    seen: set[str] = set()
    ledger_fingerprints: set[str] = set()
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            errors.append(f"Ledger entry {index} must be an object.")
            continue
        fingerprint = entry.get("fingerprint")
        if not isinstance(fingerprint, str) or not fingerprint:
            errors.append(f"Ledger entry {index} has no fingerprint.")
            continue
        if fingerprint in seen:
            errors.append(f"Duplicate ledger fingerprint: {fingerprint}")
        seen.add(fingerprint)
        ledger_fingerprints.add(fingerprint)
        owner = entry.get("owner_slice")
        if owner not in ALLOWED_OWNER_SLICES:
            errors.append(f"Invalid owner slice {owner!r}: {fingerprint}")
        if entry.get("disposition") != "migrate":
            errors.append(f"Non-migrate disposition: {fingerprint}")
        finding = current.get(fingerprint)
        if finding is not None:
            expected_owner = _owner_slice(finding)
            if owner in ALLOWED_OWNER_SLICES and owner != expected_owner:
                errors.append(f"Owner mismatch for {fingerprint}: expected {expected_owner}, got {owner}")
            expected = (finding.path, finding.rule, finding.token, finding.occurrence)
            actual = (entry.get("path"), entry.get("rule"), entry.get("token"), entry.get("occurrence", 1))
            if actual != expected:
                errors.append(f"Ledger metadata mismatch: {fingerprint}")
    for fingerprint in sorted(current.keys() - ledger_fingerprints):
        errors.append(f"Unowned finding: {fingerprint}")
    for fingerprint in sorted(ledger_fingerprints - current.keys()):
        errors.append(f"Stale ledger entry: {fingerprint}")
    return errors


def validate_empty(findings: Iterable[Finding], ledger: dict) -> list[str]:
    finding_list = list(findings)
    entries = ledger.get("findings", []) if isinstance(ledger, dict) else []
    errors: list[str] = []
    if finding_list:
        errors.append(f"Platform boundary still has {len(finding_list)} finding(s).")
    if entries:
        errors.append(f"Migration ledger still has {len(entries)} finding(s).")
    return errors


def _resolve(root: Path, path: Path) -> Path:
    return path if path.is_absolute() else root / path


def _load_ledger(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError("ledger root must be a JSON object")
    return value


def _print_summary(
    findings: list[Finding],
    ledger: dict | None = None,
    classification: ExceptionClassification | None = None,
) -> None:
    by_rule = Counter(finding.rule for finding in findings)
    by_owner = Counter(_owner_slice(finding) for finding in findings)
    print(f"Scanned findings: {len(findings)}")
    print("By rule: " + (", ".join(f"{key}={by_rule[key]}" for key in sorted(by_rule)) or "none"))
    print("By owner: " + (", ".join(f"{key}={by_owner[key]}" for key in sorted(by_owner)) or "none"))
    if classification is not None:
        print(f"Audited exceptions: {len(classification.audited_fingerprints)}")
        print(f"Unresolved findings: {len(classification.unresolved_findings)}")
    if ledger is not None:
        entries = ledger.get("findings", [])
        print(f"Ledger findings: {len(entries) if isinstance(entries, list) else 'invalid'}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--write-ledger", type=Path)
    parser.add_argument("--check-ledger", type=Path)
    parser.add_argument("--check-exceptions", type=Path)
    parser.add_argument("--require-empty", action="store_true")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    raw_findings = scan_tree(root)
    findings = raw_findings
    ledger: dict | None = None
    classification: ExceptionClassification | None = None
    errors: list[str] = []
    if args.write_ledger:
        ledger_path = _resolve(root, args.write_ledger)
        if ledger_path.exists():
            errors.append(f"Refusing to overwrite existing ledger: {ledger_path}")
        else:
            ledger = build_ledger(findings)
            ledger_path.parent.mkdir(parents=True, exist_ok=True)
            ledger_path.write_text(json.dumps(ledger, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
            print(f"Wrote ledger: {ledger_path}")
    if args.check_exceptions:
        exceptions_path = _resolve(root, args.check_exceptions)
        try:
            exceptions = _load_ledger(exceptions_path)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            errors.append(f"Cannot load exceptions {exceptions_path}: {error}")
        else:
            classification = validate_audited_exceptions(raw_findings, exceptions, root)
            errors.extend(classification.errors)
            findings = classification.unresolved_findings
    if args.check_ledger:
        ledger_path = _resolve(root, args.check_ledger)
        try:
            ledger = _load_ledger(ledger_path)
        except (OSError, ValueError, json.JSONDecodeError) as error:
            errors.append(f"Cannot load ledger {ledger_path}: {error}")
        else:
            if classification is not None:
                errors.extend(validate_no_overlap(ledger, classification.audited_fingerprints))
            errors.extend(compare_ledger(findings, ledger))
    if args.require_empty:
        if ledger is None:
            ledger_path = root / DEFAULT_LEDGER
            try:
                ledger = _load_ledger(ledger_path) if ledger_path.exists() else {}
            except (OSError, ValueError, json.JSONDecodeError) as error:
                errors.append(f"Cannot load ledger {ledger_path}: {error}")
                ledger = {}
        errors.extend(validate_empty(findings, ledger))
    _print_summary(raw_findings, ledger, classification)
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
