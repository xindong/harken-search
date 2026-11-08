import hashlib
import hmac
import json
import re
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import pack_engine  # noqa: E402


def sample_manifest():
    return {
        "version": 1,
        "groups": [{"id": "core", "name": "Core", "order": 0}],
        "features": [
            {
                "id": "core_waveform",
                "required_edition": "base",
                "dlls": [],
                "default_enabled": True,
                "internal_test_default": True,
            },
            {
                "id": "pro_export",
                "required_edition": "pro",
                "dlls": ["shared.dll", "codec.dll"],
                "default_enabled": True,
                "internal_test_default": True,
            },
            {
                "id": "pro_meter",
                "required_edition": "pro",
                "dlls": ["shared.dll", "meter.dll"],
                "default_enabled": False,
                "internal_test_default": False,
            },
            {
                "id": "ai_engine",
                "required_edition": "pro",
                "dlls": ["ai.dll"],
                "default_enabled": True,
                "internal_test_default": False,
            },
            {
                "id": "ultimate_cover_wall",
                "required_edition": "ultimate",
                "dlls": ["ultimate.dll"],
                "default_enabled": True,
                "internal_test_default": False,
            },
        ],
    }


class PackEngineTests(unittest.TestCase):
    def test_package_script_requires_private_cuevi_feedback_configuration(self):
        script = (ROOT / "scripts" / "package.ps1").read_text(encoding="utf-8")
        required = (
            "AIRS_CUEVI_FEEDBACK_ENDPOINT",
            "AIRS_CUEVI_FEEDBACK_KEY",
            "AIRS_CUEVI_PROJECT_ID",
            "AIRS_CUEVI_BOARD_ID",
            "AIRS_CUEVI_MILESTONE_ID",
        )
        for name in required:
            with self.subTest(name=name):
                self.assertRegex(script, rf"{name}\s*=\s*\$env:{name}")
        self.assertIn('throw "$name is required for a feedback-enabled package"', script)
        self.assertIn('"-D$($entry.Key)=$($entry.Value)"', script)

    def test_package_script_never_prints_the_restricted_feedback_key(self):
        script = (ROOT / "scripts" / "package.ps1").read_text(encoding="utf-8")
        output_lines = [
            line
            for line in script.splitlines()
            if "Write-Host" in line or "Write-Output" in line or "Out-File" in line
        ]
        self.assertNotIn("AIRS_CUEVI_FEEDBACK_KEY", "\n".join(output_lines))

    def test_package_script_includes_generated_integrity_sidecars(self):
        script = (ROOT / "scripts" / "package.ps1").read_text(encoding="utf-8")
        copy_block = script.split("# Copy exe + all DLLs", 1)[1].split(
            "# Copy assets subdirectories", 1
        )[0]
        self.assertIn('".sig"', copy_block)

    def test_cuevi_feedback_test_target_selects_platform_sources_and_libraries(self):
        cmake = (ROOT / "cmake" / "harken_search_behavior_tests.cmake").read_text(encoding="utf-8")
        block = cmake.split("add_executable(TestCueviFeedbackClient", 1)[1].split(
            "add_test(NAME TestCueviFeedbackClient", 1
        )[0]
        self.assertIn("if(WIN32)", block)
        self.assertIn("src/platform/system_win32.cpp", block)
        self.assertIn("src/platform/text_encoding_win32.cpp", block)
        self.assertIn("src/platform/text_encoding_std.cpp", block)
        self.assertIn("target_link_libraries(TestCueviFeedbackClient PRIVATE wininet", block)
        self.assertLess(block.index("if(WIN32)"), block.index("target_link_libraries(TestCueviFeedbackClient PRIVATE wininet"))

    def test_edition_ceiling_filters_by_required_edition(self):
        manifest = sample_manifest()

        self.assertEqual(pack_engine.get_edition_ceiling(manifest, "base"), ["core_waveform"])
        self.assertEqual(
            pack_engine.get_edition_ceiling(manifest, "pro"),
            ["core_waveform", "pro_export", "pro_meter", "ai_engine"],
        )
        self.assertEqual(
            pack_engine.get_edition_ceiling(manifest, "ultimate"),
            [
                "core_waveform",
                "pro_export",
                "pro_meter",
                "ai_engine",
                "ultimate_cover_wall",
            ],
        )

    def test_default_presets_and_required_dlls_are_stable(self):
        manifest = sample_manifest()

        self.assertEqual(pack_engine.get_internal_test_defaults(manifest), ["core_waveform", "pro_export"])
        self.assertEqual(
            pack_engine.get_default_enabled(manifest),
            ["core_waveform", "pro_export", "ai_engine", "ultimate_cover_wall"],
        )
        self.assertEqual(
            pack_engine.collect_required_dlls(manifest, ["pro_meter", "pro_export", "ai_engine"]),
            ["shared.dll", "codec.dll", "meter.dll", "ai.dll"],
        )

    def test_features_json_and_signature_contract(self):
        payload = json.loads(
            pack_engine.generate_features_json(
                ["pro_export", "core_waveform", "pro_export"], "pro", "2.3.0"
            )
        )

        self.assertEqual(payload["edition"], "pro")
        self.assertEqual(payload["edition_level"], 1)
        self.assertEqual(payload["enabled_features"], ["core_waveform", "pro_export", "pro_export"])
        self.assertEqual(payload["version"], "2.3.0")
        self.assertRegex(payload["generated_at"], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")

        secret = bytes(range(32))
        data = b'{"edition":"pro"}'
        self.assertEqual(pack_engine.sign_json(data, secret), hmac.new(secret, data, hashlib.sha256).digest())

    def test_pack_writes_signed_zip_and_filters_by_edition(self):
        manifest = sample_manifest()
        secret = b"x" * 32

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            release_dir = root / "Release"
            output_dir = root / "dist"
            release_dir.mkdir()
            (release_dir / "HarkenSearch.exe").write_bytes(b"exe")
            (release_dir / "HarkenSearch.exe.sig").write_bytes(b"sig")
            (release_dir / "HarkenSearchBootstrap.exe").write_bytes(b"bootstrap")
            (release_dir / "reaper_harken.dll").write_bytes(b"reaper-bridge")
            pack_engine.write_release_notes_file(release_dir, {
                "schema_version": 2,
                "releases": [{
                    "version": "2.3.0", "release_date": "2026-10-19",
                    "headline": "修复更新状态", "contributors": ["Harken Search Team"],
                    "items": [{"category": "fix", "text": "修复更新提示"}],
                }],
            })
            # Legacy decoys prove that publishing selects Aris artifacts rather
            # than succeeding by silently falling back to the former identity.
            (release_dir / "Auro.exe").write_bytes(b"legacy-exe")
            (release_dir / "Auro.exe.sig").write_bytes(b"legacy-sig")
            (release_dir / "lucide.ttf").write_bytes(b"font")
            (release_dir / "splash_beta.png").write_bytes(b"splash")
            (release_dir / "harken_thesaurus.json").write_text("{}", encoding="utf-8")
            (release_dir / "auro_thesaurus.json").write_text("{}", encoding="utf-8")
            assets_dir = release_dir / "assets" / "icons" / "brand"
            assets_dir.mkdir(parents=True)
            (assets_dir / "auro.png").write_bytes(b"png")
            # Reusing an older build must not reintroduce the obsolete asset
            # parents that the original 42 updater cannot create.
            for relative in ("contributors/cindy.png","community/feishu-qr.png","community/wecom-qr.png"):
                stale=release_dir/"assets/icons"/relative
                stale.parent.mkdir(parents=True,exist_ok=True)
                stale.write_bytes(b"stale generated asset")
            for name in ["shared.dll", "codec.dll", "meter.dll", "ai.dll", "ultimate.dll"]:
                (release_dir / name).write_bytes(name.encode("ascii"))
            (release_dir / "onnxruntime.dll").write_bytes(b"onnx-runtime")
            (release_dir / "THIRD_PARTY_COMPONENTS.json").write_text(
                json.dumps({"components": [
                    {"name": "ONNX Runtime", "distributed": True},
                    {"name": "LAION CLAP HTSAT unfused audio encoder", "distributed": True},
                ]}), encoding="utf-8",
            )
            models_dir = release_dir / "models"
            models_dir.mkdir()
            (models_dir / "model.bin").write_bytes(b"model")
            clap_dir = models_dir / "clap"
            clap_dir.mkdir()
            for name in ("clap-audio.onnx", "clap-mel-filters.f64", "clap-window.f64"):
                (clap_dir / name).write_bytes(name.encode("ascii"))

            zip_path = pack_engine.pack(
                release_dir=release_dir,
                output_dir=output_dir,
                edition="pro",
                enabled_ids=[
                    "core_waveform",
                    "pro_export",
                    "pro_meter",
                    "ai_engine",
                    "ultimate_cover_wall",
                ],
                version="2.3.0",
                secret=secret,
                manifest=manifest,
                extra_files=["reaper_harken.dll"],
            )

            self.assertTrue(zip_path.exists())
            self.assertRegex(zip_path.name, r"^HarkenSearch_v2\.3\.0_Pro_\d{8}\.zip$")
            self.assertEqual((release_dir / "harken_features.sig").read_bytes(), pack_engine.sign_json(
                (release_dir / "harken_features.json").read_bytes(), secret
            ))

            with zipfile.ZipFile(zip_path, "r") as zf:
                archive_names = zf.namelist()
                self.assertEqual(archive_names.count("reaper_harken.dll"), 1)
                self.assertEqual(len(archive_names), len(set(archive_names)))
                names = set(archive_names)
                self.assertTrue({"HarkenSearch.exe", "HarkenSearch.exe.sig", "reaper_harken.dll", "harken_features.json", "harken_features.sig"}.issubset(names))
                self.assertEqual(zf.read("reaper_harken.dll"), b"reaper-bridge")
                self.assertTrue({"lucide.ttf", "splash_beta.png", "harken_thesaurus.json"}.issubset(names))
                self.assertTrue({"Auro.exe", "Auro.exe.sig", "auro_features.json", "auro_features.sig", "auro_thesaurus.json"}.isdisjoint(names))
                self.assertIn("assets/icons/brand/auro.png", names)
                self.assertFalse(any(name.startswith(("assets/icons/contributors/","assets/icons/community/")) for name in names))
                self.assertTrue((release_dir/"assets/icons/contributors/cindy.png").is_file())
                self.assertTrue({"shared.dll", "codec.dll", "meter.dll", "ai.dll"}.issubset(names))
                self.assertIn("models/model.bin", names)
                self.assertEqual(zf.read("onnxruntime.dll"), b"onnx-runtime")
                self.assertTrue({
                    "models/clap/clap-audio.onnx", "models/clap/clap-mel-filters.f64",
                    "models/clap/clap-window.f64",
                }.issubset(names))
                self.assertIn("aris_release_notes.json", names)
                self.assertNotIn("ultimate.dll", names)

                manual_path = "docs/HarkenSearch-Manual-zh-CN.pdf"
                self.assertIn(manual_path, zf.namelist())
                self.assertEqual(zf.read(manual_path), (ROOT / "docs/manual/HarkenSearch-Manual-zh-CN.pdf").read_bytes())
                features_payload = json.loads(zf.read("harken_features.json").decode("utf-8"))
                self.assertEqual(features_payload["edition"], "pro")
                self.assertNotIn("ultimate_cover_wall", features_payload["enabled_features"])
                self.assertIn("ai_engine", features_payload["enabled_features"])
                self.assertEqual(len(zf.read("harken_features.sig")), 32)

    def test_pack_fails_before_writing_zip_when_reaper_bridge_is_missing(self):
        manifest = sample_manifest()

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            release_dir = root / "Release"
            output_dir = root / "dist"
            release_dir.mkdir()
            (release_dir / "HarkenSearch.exe").write_bytes(b"exe")
            (release_dir / "HarkenSearch.exe.sig").write_bytes(b"sig")
            pack_engine.write_release_notes_file(release_dir, {
                "schema_version": 2,
                "releases": [{
                    "version": "2.3.0", "release_date": "2026-10-19",
                    "headline": "修复更新状态", "contributors": ["Harken Search Team"],
                    "items": [{"category": "fix", "text": "修复更新提示"}],
                }],
            })

            with self.assertRaises(FileNotFoundError) as ctx:
                pack_engine.pack(
                    release_dir=release_dir,
                    output_dir=output_dir,
                    edition="base",
                    enabled_ids=[],
                    version="2.3.0",
                    secret=b"x" * 32,
                    manifest=manifest,
                )

            self.assertIn("reaper_harken.dll", str(ctx.exception))
            self.assertEqual(list(output_dir.glob("*.zip")), [])

    def test_pack_rejects_missing_configured_similarity_runtime_before_zip(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            release_dir = root / "Release"
            output_dir = root / "dist"
            release_dir.mkdir()
            pack_engine.write_release_notes_file(release_dir, {
                "schema_version": 2,
                "releases": [{
                    "version": "2.3.0", "release_date": "2026-10-19",
                    "headline": "Similarity package test", "contributors": ["Harken Search Team"],
                    "items": [{"category": "fix", "text": "Keep configured runtime bundled"}],
                }],
            })
            (release_dir / "THIRD_PARTY_COMPONENTS.json").write_text(
                json.dumps({"components": [
                    {"name": "ONNX Runtime", "distributed": True},
                ]}), encoding="utf-8",
            )
            with self.assertRaisesRegex(FileNotFoundError, "onnxruntime.dll"):
                pack_engine.pack(
                    release_dir=release_dir, output_dir=output_dir, edition="base",
                    enabled_ids=[], version="2.3.0", secret=b"x" * 32,
                    manifest=sample_manifest(),
                )
            self.assertEqual(list(output_dir.glob("*.zip")), [])

    def test_publish_surface_uses_aris_product_identity(self):
        self.assertEqual(pack_engine.RELEASE_HISTORY_JSON_PATH.name, "harken_release_history.json")
        self.assertEqual(pack_engine.INSTALLER_TEMPLATE_PATH.name, "HarkenSearch_Setup.iss")
        self.assertEqual(pack_engine.GENERATED_INSTALLER_PATH.name, "HarkenSearch_Setup_generated.iss")
        self.assertEqual(pack_engine.RELEASE_NOTES_FILENAME, "aris_release_notes.json")
        self.assertEqual(
            pack_engine.ALWAYS_INCLUDE,
            ["HarkenSearch.exe", "HarkenSearch.exe.sig", "HarkenSearchBootstrap.exe", "reaper_harken.dll"],
        )
        self.assertIn("harken_thesaurus.json", pack_engine.RUNTIME_INCLUDE)
        self.assertEqual(
            pack_engine.render_release_history_markdown({"schema_version": 1, "releases": []}).splitlines()[0],
            "# Aris Release History",
        )

    def test_real_pack_manifest_is_schema_safe_for_packer_ui(self):
        manifest = pack_engine.load_manifest(ROOT / "tools" / "pack_manifest.json")
        self.assertEqual(manifest["version"], 1)

        group_ids = [group["id"] for group in manifest["groups"]]
        self.assertEqual(len(group_ids), len(set(group_ids)))
        self.assertEqual([group["order"] for group in manifest["groups"]], sorted(group["order"] for group in manifest["groups"]))

        feature_ids = [feature["id"] for feature in manifest["features"]]
        self.assertGreaterEqual(len(feature_ids), 30)
        self.assertEqual(len(feature_ids), len(set(feature_ids)))
        self.assertNotIn("ai_cover_gen", feature_ids)
        self.assertNotIn("ai_cover_gen", pack_engine.AI_FEATURE_IDS)
        self.assertTrue(pack_engine.AI_FEATURE_IDS.issubset(set(feature_ids)))

        group_set = set(group_ids)
        seen_names = set()
        for feature in manifest["features"]:
            with self.subTest(feature=feature["id"]):
                self.assertRegex(feature["id"], r"^[a-z][a-z0-9_]*$")
                self.assertIn(feature.get("group"), group_set)
                self.assertIn(feature.get("required_edition", "base"), pack_engine.EDITION_MAP)
                self.assertIsInstance(feature.get("default_enabled", True), bool)
                self.assertIsInstance(feature.get("internal_test_default", False), bool)
                self.assertTrue(feature.get("name"))
                self.assertTrue(feature.get("name_en"))
                self.assertNotIn(feature["name_en"], seen_names)
                seen_names.add(feature["name_en"])
                for dll in feature.get("dlls", []):
                    self.assertEqual(Path(dll).name, dll)
                    self.assertTrue(dll.lower().endswith(".dll"))

        base = set(pack_engine.get_edition_ceiling(manifest, "base"))
        pro = set(pack_engine.get_edition_ceiling(manifest, "pro"))
        ultimate = set(pack_engine.get_edition_ceiling(manifest, "ultimate"))
        self.assertTrue(base.issubset(pro))
        self.assertTrue(pro.issubset(ultimate))

    def test_load_manifest_rejects_invalid_manifest_contract(self):
        invalid_manifest = {
            "version": 1,
            "groups": [{"id": "core", "name": "Core", "order": 0}],
            "features": [
                {
                    "id": "ai_engine",
                    "name": "AI Engine",
                    "name_en": "AI Engine",
                    "group": "core",
                    "required_edition": "pro",
                    "dlls": ["models/ai.dll"],
                    "default_enabled": True,
                    "internal_test_default": True,
                },
                {
                    "id": "ai_engine",
                    "name": "AI Engine Copy",
                    "name_en": "AI Engine Copy",
                    "group": "core",
                    "required_edition": "pro",
                    "dlls": [],
                    "default_enabled": True,
                    "internal_test_default": False,
                },
            ],
        }

        with tempfile.TemporaryDirectory() as temp_dir:
            manifest_path = Path(temp_dir) / "pack_manifest.json"
            manifest_path.write_text(json.dumps(invalid_manifest), encoding="utf-8")
            with self.assertRaises(ValueError) as ctx:
                pack_engine.load_manifest(manifest_path)

        message = str(ctx.exception)
        self.assertIn("duplicate feature id: ai_engine", message)
        self.assertIn("DLL entry must be a file-local .dll basename", message)
        self.assertIn("missing AI feature ids", message)

    def test_real_manifest_feature_ids_cover_runtime_gates_or_documented_aliases(self):
        manifest = pack_engine.load_manifest(ROOT / "tools" / "pack_manifest.json")
        manifest_ids = {feature["id"] for feature in manifest["features"]}
        product_edition_cpp = (ROOT / "src" / "ProductEdition.cpp").read_text(encoding="utf-8")
        runtime_ids = set(re.findall(r'\{"([a-z0-9_]+)",\s*Edition::', product_edition_cpp))

        documented_aliases = {
            "batch_export": "transfer_engine",
            "batch_rename": "naming_script",
            "drag_to_daw": "export_workflow",
            "multi_library": "multi_db",
            "reverse_playback": "advanced_playback",
            "spectrum_analyzer": "loudness",
            "wwise_transfer": "transfer_engine",
        }

        runtime_aliases = dict(re.findall(
            r'\{"([a-z0-9_]+)",\s*"([a-z0-9_]+)"\}',
            product_edition_cpp,
        ))
        for runtime_id, manifest_id in documented_aliases.items():
            with self.subTest(runtime_alias=runtime_id):
                self.assertEqual(runtime_aliases.get(runtime_id), manifest_id)

        unresolved = {
            feature_id
            for feature_id in runtime_ids
            if feature_id not in manifest_ids and documented_aliases.get(feature_id) not in manifest_ids
        }
        self.assertEqual(unresolved, set())


if __name__ == "__main__":
    unittest.main()
