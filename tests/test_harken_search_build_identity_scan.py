import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"
INSTALLER_NEW = ROOT / "installer" / "HarkenSearch_Setup.iss"
INSTALLER_OLD = ROOT / "installer" / "Aris_Setup.iss"
MANIFEST = ROOT / "resources" / "HarkenSearch.manifest"
RESOURCES_RC = ROOT / "resources.rc"
QA_PACKAGE = ROOT / "scripts" / "package_qa_exe.ps1"

ALLOWED_OLD_IN_CMAKE = (
    re.compile(r"src/bootstrap/ArisBootstrap[A-Za-z0-9]*\.(cpp|h)"),
    re.compile(r"src/AuroProToolsBridgeMain\.cpp"),
    re.compile(r"\bArisBootstrapCore\b"),
    re.compile(r"\bArisUpdateNoticeNativeQa\b"),
    re.compile(r"\bArisLocalizationGenerated\b"),
    re.compile(r"\bArisRuntimePairIdentity\b"),
    re.compile(r"resources/HarkenSearch\.manifest"),
)

FORBIDDEN_CMAKE_TARGET = (
    ("project(Aris", re.compile(r"project\(\s*Aris\b")),
    ("add_executable(Aris)", re.compile(r"add_executable\(\s*Aris\s")),
    ("add_executable(ArisBootstrap)", re.compile(r"add_executable\(\s*ArisBootstrap\b")),
    ("add_executable(AuroProToolsBridge)", re.compile(r"add_executable\(\s*AuroProToolsBridge\b")),
    ("TARGET_FILE:Aris", re.compile(r"TARGET_FILE:Aris(?![A-Za-z])")),
    ("TARGET_FILE_DIR:Aris", re.compile(r"TARGET_FILE_DIR:Aris(?![A-Za-z])")),
    ("TARGET_FILE:ArisBootstrap", re.compile(r"TARGET_FILE:ArisBootstrap\b")),
    ("TARGET_FILE:AuroProToolsBridge", re.compile(r"TARGET_FILE(?:_DIR)?:AuroProToolsBridge\b")),
    ("TARGET Aris", re.compile(r"TARGET\s+Aris(?![A-Za-z])")),
    ("target command Aris", re.compile(r"(?:target_(?:compile_definitions|compile_options|compile_features|link_libraries|link_options|include_directories)|set_target_properties|add_dependencies)\(\s*Aris(?![A-Za-z])")),
    ("set_property TARGET Aris", re.compile(r"set_property\(\s*TARGET\s+Aris(?![A-Za-z])")),
    ("Aris.exe", re.compile(r"Aris\.exe\b")),
    ("ArisBootstrap.exe", re.compile(r"ArisBootstrap\.exe\b")),
    ("AuroProToolsBridge.exe", re.compile(r"AuroProToolsBridge\.exe\b")),
)

FORBIDDEN_INSTALLER = (
    re.compile(r"Aris\.exe\b"),
    re.compile(r"ArisBootstrap\.exe\b"),
    re.compile(r"AuroProToolsBridge\.exe\b"),
    re.compile(r"OutputBaseFilename=Aris_Setup"),
    re.compile(r"#define MyAppExeName " + chr(34) + r"Aris" + chr(92) + r".exe" + chr(34)),
    re.compile(r"Aris\.ico"),
    re.compile(r"Airs\.ico"),
)

REQUIRED_CMAKE = (
    "project(HarkenSearch",
    "add_executable(HarkenSearch",
    "add_executable(HarkenSearchBootstrap",
    "add_executable(HarkenSearchProToolsBridge",
    "OUTPUT_NAME HarkenSearch",
    "OUTPUT_NAME HarkenSearchBootstrap",
    "OUTPUT_NAME HarkenSearchProToolsBridge",
    "HarkenSearch.exe",
    "HarkenSearchBootstrap.exe",
)

REQUIRED_INSTALLER = (
    '#define MyAppName "Harken Search"',
    '#define MyAppExeName "HarkenSearch.exe"',
    "OutputBaseFilename=HarkenSearch_Setup_v",
    "HarkenSearch.exe",
    "HarkenSearchBootstrap.exe",
    "HarkenSearchProToolsBridge.exe",
    r"SetupIconFile=..\HarkenSearch.ico",
)


def _mask_allowed(text):
    masked = text
    for rx in ALLOWED_OLD_IN_CMAKE:
        masked = rx.sub(lambda m: " " * len(m.group(0)), masked)
    return masked


class HarkenSearchBuildIdentityScanTests(unittest.TestCase):
    def test_build_and_qa_package_do_not_ship_auro_executable_alias(self):
        cmake = CMAKE.read_text(encoding="utf-8")
        qa_package = QA_PACKAGE.read_text(encoding="utf-8")
        for filename in ("Auro.exe", "Auro.exe.sig"):
            with self.subTest(filename=filename):
                self.assertNotIn(filename, cmake)
                self.assertNotIn('"' + filename + '"', qa_package)

    def test_cmake_installer_resources_no_longer_emit_old_build_identity(self):
        cmake = CMAKE.read_text(encoding="utf-8")
        masked = _mask_allowed(cmake)
        hits = []
        for label, rx in FORBIDDEN_CMAKE_TARGET:
            for match in rx.finditer(masked):
                line = masked.count(chr(10), 0, match.start()) + 1
                hits.append("CMakeLists.txt:%d:%s:%s" % (line, label, match.group(0)))
        self.assertEqual(hits, [], "old CMake build identity remains:" + chr(10) + chr(10).join(hits))
        missing = [token for token in REQUIRED_CMAKE if token not in cmake]
        self.assertEqual(missing, [], "required HarkenSearch CMake identity missing: " + ", ".join(missing))
        self.assertFalse(INSTALLER_OLD.exists(), "installer/Aris_Setup.iss must be renamed")
        self.assertTrue(INSTALLER_NEW.is_file(), "installer/HarkenSearch_Setup.iss must exist")
        installer = INSTALLER_NEW.read_text(encoding="utf-8")
        legacy_start = installer.index("function LegacyProductIsRunning(")
        legacy_end = installer.index("function IsUserProfileInstallDir(", legacy_start)
        installer_hits = []
        for rx in FORBIDDEN_INSTALLER:
            for match in rx.finditer(installer):
                line = installer.count(chr(10), 0, match.start()) + 1
                raw = installer.splitlines()[line - 1]
                if (legacy_start <= match.start() < legacy_end and
                    match.group(0) in {"Auro.exe", "Aris.exe", "ArisBootstrap.exe"} and
                    "IsFileLocked(AddBackslash(InstallLocation) + '" + match.group(0) + "')" in raw):
                    continue
                installer_hits.append("HarkenSearch_Setup.iss:%d:%s" % (line, match.group(0)))
        self.assertEqual(installer_hits, [], "old installer identity remains:" + chr(10) + chr(10).join(installer_hits))
        missing_installer = [token for token in REQUIRED_INSTALLER if token not in installer]
        self.assertEqual(missing_installer, [], "required installer identity missing: " + ", ".join(missing_installer))
        manifest = MANIFEST.read_text(encoding="utf-8")
        self.assertNotIn('name="xindong.aris"', manifest)
        self.assertIn('name="xindong.harkensearch"', manifest)
        self.assertIn("<description>Harken Search</description>", manifest)
        rc = RESOURCES_RC.read_text(encoding="utf-8-sig")
        self.assertIn('VALUE "OriginalFilename", "HarkenSearch.exe"', rc)
        self.assertIn('VALUE "ProductName", "Harken Search"', rc)
        self.assertNotIn("Aris.exe", rc)

        self.assertIn('1 ICON "HarkenSearch.ico"', rc)
        self.assertNotIn("Aris.ico", rc)
        self.assertNotIn("Airs.ico", rc)

    def test_resource_references_drop_old_brand_ico_filenames(self):
        icon = ROOT / "HarkenSearch.ico"
        self.assertTrue(icon.is_file(), "HarkenSearch.ico must exist at repo root")
        self.assertFalse((ROOT / "Aris.ico").is_file(), "Aris.ico must be renamed to HarkenSearch.ico")
        skip = {
            "tests/test_harken_search_build_identity_scan.py",
            "tests/test_harken_search_visible_copy_scan.py",
            "tests/test_public_repo_manifest.py",
        }
        roots = [
            ROOT / "resources.rc",
            ROOT / "CMakeLists.txt",
            ROOT / "installer",
            ROOT / "src",
            ROOT / "tools",
            ROOT / "scripts",
            ROOT / ".github",
            ROOT / "tests",
        ]
        hits = []
        binary = {".ico", ".png", ".jpg", ".jpeg", ".gif", ".dll", ".exe", ".lib", ".obj", ".pdb", ".zip", ".ttf", ".wav"}
        for root in roots:
            paths = [root] if root.is_file() else list(root.rglob("*"))
            for path in paths:
                if not path.is_file():
                    continue
                rel = path.relative_to(ROOT).as_posix()
                if rel in skip:
                    continue
                if path.suffix.lower() in binary:
                    continue
                try:
                    body = path.read_text(encoding="utf-8")
                except UnicodeDecodeError:
                    try:
                        body = path.read_text(encoding="utf-8-sig")
                    except UnicodeDecodeError:
                        continue
                for name in ("Aris.ico", "Airs.ico"):
                    if name in body:
                        for line_no, line in enumerate(body.splitlines(), 1):
                            if name in line:
                                hits.append("%s:%d:%s" % (rel, line_no, name))
        self.assertEqual(hits, [], "old brand ico filename remains:" + chr(10) + chr(10).join(hits))


if __name__ == "__main__":
    unittest.main()

