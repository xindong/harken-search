# Windows bootstrap executable entry audit

Reviewed 2026-11-15. This source is the wWinMain entry for the separate
HarkenSearchBootstrap helper, not shared application code. CMake declares that
target inside the Windows-only block (if(WIN32)), and the main application
depends on that target only in the same Windows configuration. macOS does not
compile or link this entry source.

The exact native calls below are used by the helper's Windows process and
invocation checks. CloseHandle releases the process/file handles it opens;
CreateFileW and GetFileInformationByHandleEx validate invocation paths and
file identity; MultiByteToWideChar converts inherited UTF-8 invocation
arguments; WaitForSingleObject serializes helper execution through the named
mutex. windows.h supplies the Windows API declarations. The source remains a
Windows-specific entrypoint; these calls are not being presented as portable
services. Any future movement of this target or source outside the WIN32
configuration invalidates this review and must fail the exact-fingerprint test.

The audited register contains exactly these eight findings:

| Line | Rule | API | Fingerprint suffix |
| ---: | --- | --- | --- |
| 10 | native-include | windows.h | d395cd89463e8f58 |
| 153 | native-symbol | CloseHandle | 97c6d24b6ef1dcfe |
| 158 | native-symbol | MultiByteToWideChar | 4c3018a8eb2b8db2 |
| 164 | native-symbol | MultiByteToWideChar | 5bfb8a7166f50da5 |
| 168 | native-symbol | CreateFileW | 233b50a7454c3fec |
| 174 | native-symbol | GetFileInformationByHandleEx | 532e0fb6cb253794 |
| 176 | native-symbol | CloseHandle | fbda12bae9f177bb |
| 192 | native-symbol | WaitForSingleObject | fe8dd7054cfd75c4 |

This audit only classifies the platform-specific bootstrap entry. It does not
verify Windows behavior or change the macOS implementation.
