#!/usr/bin/env python3
"""RQ-20260909-02 knife-1 rework: baseline (no-regression) language purity gate.

Stock mixed / hardcoded / Japanese-missing copy is recorded in a committed
baseline, so the gate is green on the current repo. It FAILS only when a NEW
violation appears, reporting each with a locatable description. Do not rewrite
visible copy to make these pass, and do not weaken the scanner.
"""

from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import localization_purity  # noqa: E402

BASELINE_PATH = ROOT / "locale" / "aris.ui.strings.purity-baseline.json"


class LocalizationPurityBaselineTests(unittest.TestCase):
    def test_japanese_column_accepts_kanji_only_translation(self) -> None:
        import localization_purity

        ledger = {
            "entries": [
                {
                    "id": "Set_Settings",
                    "zh": "设置",
                    "en": "Settings",
                    "ja": "設定",
                }
            ]
        }

        findings = localization_purity.find_ledger_mixes(ledger)

        self.assertEqual(findings, [])

    def test_japanese_column_rejects_simplified_chinese_copy(self) -> None:
        import localization_purity

        ledger = {
            "entries": [
                {
                    "id": "Set_Settings",
                    "zh": "设置",
                    "en": "Settings",
                    "ja": "设置",
                }
            ]
        }

        findings = localization_purity.find_ledger_mixes(ledger)

        self.assertEqual(
            [(item["id"], item["lang"]) for item in findings],
            [("Set_Settings", "ja")],
        )

    def test_japanese_column_rejects_explicit_chinese_language_label(self) -> None:
        import localization_purity

        findings = localization_purity.find_ledger_mixes(
            {
                "entries": [
                    {
                        "id": "Set_LanguageName",
                        "zh": "语言",
                        "en": "Language",
                        "ja": "简体中文",
                    }
                ]
            }
        )

        self.assertEqual([(item["id"], item["lang"]) for item in findings], [("Set_LanguageName", "ja")])

    def test_units_and_proper_nouns_are_not_mixes(self) -> None:
        self.assertFalse(localization_purity.is_language_mix("采样率 48000 Hz"))
        self.assertFalse(localization_purity.is_language_mix("共 %lld 个文件"))
        self.assertFalse(localization_purity.is_language_mix("Send to Wwise"))
        self.assertFalse(localization_purity.is_language_mix("基于 ImGui + SQLite 构建"))

    def test_parenthetical_english_in_chinese_is_a_mix(self) -> None:
        self.assertTrue(localization_purity.is_language_mix("描述 (Description):"))

    def test_imgui_id_suffix_is_not_a_fake_mix(self) -> None:
        # Knife-5 precondition: ##/### id suffix is never visible; pure-CJK
        # labels must not be flagged mixed by the suffix ASCII...
        self.assertFalse(localization_purity.is_language_mix("应用##custom"))
        self.assertFalse(localization_purity.is_language_mix("取消排队##batchstop"))
        self.assertFalse(localization_purity.is_language_mix("×##ClearKey"))
        # ...but a real mix hidden behind a suffix still fails (no weakening).
        # (复制SRT would now be clean: SRT is a ratified file extension.)
        self.assertTrue(localization_purity.is_language_mix("重新分析 Key##fx"))
        self.assertTrue(localization_purity.is_language_mix("描述 (Description)##fx"))

    def test_scanner_still_sees_labels_with_id_suffix(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            src = Path(temp_dir) / "src"
            src.mkdir()
            (src / "ZZSuffixFixture.cpp").write_text(
                '#include "imgui.h"\nvoid Draw() { ImGui::Button(u8"新建条目##zz"); }\n',
                encoding="utf-8",
            )
            texts = {item["text"] for item in localization_purity.find_hardcoded_ui(src)}
            self.assertIn("新建条目##zz", texts)

    def test_frozen_menu_labels_are_not_rewritten_this_slice(self) -> None:
        ledger = localization_purity.load_ledger()
        frozen = {entry["id"]: entry["zh"] for entry in ledger["entries"]}
        for ident, expected in localization_purity.FROZEN_ZH.items():
            self.assertEqual(frozen[ident], expected)

    def test_scanner_sees_window_tab_input_and_slider_labels(self) -> None:
        # Scanner capability regression must not depend on real repo literals that
        # ledger migration (knife-4) intentionally removes. Use synthetic fixtures
        # instead: one hardcoded literal per recognized widget family, and assert
        # the scanner sees each. Does NOT touch the scanner/baseline/gate logic.
        with tempfile.TemporaryDirectory() as temp_dir:
            src = Path(temp_dir) / "src"
            src.mkdir()
            (src / "FixtureControls.cpp").write_text(
                "\n".join(
                    [
                        "#include \"imgui.h\"",
                        "void Fixture() {",
                        "    ImGui::Begin(\"合成窗口标题\");",
                        "    ImGui::BeginTabItem(\"合成选项卡\");",
                        "    ImGui::InputText(\"合成输入标签##fx\", buf, sizeof(buf));",
                        "    ImGui::SliderInt(\"合成滑条标签##fx\", &v, 0, 10);",
                        "    ImGui::Button(\"合成按钮\");",
                        "    ImGui::MenuItem(\"合成菜单项\");",
                        "    ImGui::TextUnformatted(\"合成文本\");",
                        "    ImGui::TextDisabled(\"合成禁用文本\");",
                        "    ImGui::Selectable(\"合成列表项\");",
                        "    ImGui::Checkbox(\"合成复选框\", &b);",
                        "    ImGui::RadioButton(\"合成单选项\", &g, 0);",
                        "    ImGui::Combo(\"合成下拉框\", &c, items);",
                        "    ImGui::LabelText(\"合成标签文本\", \"%d\", v);",
                        "    ImGui::SetTooltip(\"合成悬浮提示\");",
                        "    ImGui::End();",
                        "}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            findings = localization_purity.find_hardcoded_ui(src)
            by_func: dict[str, set[str]] = {}
            for item in findings:
                by_func.setdefault(item["func"], set()).add(item["text"])
            expected = {
                "Begin": "合成窗口标题",
                "BeginTabItem": "合成选项卡",
                "InputText": "合成输入标签##fx",
                "SliderInt": "合成滑条标签##fx",
                "Button": "合成按钮",
                "MenuItem": "合成菜单项",
                "TextUnformatted": "合成文本",
                "TextDisabled": "合成禁用文本",
                "Selectable": "合成列表项",
                "Checkbox": "合成复选框",
                "RadioButton": "合成单选项",
                "Combo": "合成下拉框",
                "LabelText": "合成标签文本",
                "SetTooltip": "合成悬浮提示",
            }
            for func, literal in expected.items():
                self.assertIn(literal, by_func.get(func, set()), f"scanner missed {func}")


    def test_baseline_matches_current_findings(self) -> None:
        self.assertTrue(BASELINE_PATH.is_file(), "baseline file must be committed")
        payload = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
        localization_purity.check_baseline_growth(payload)
        stored = set(localization_purity.load_baseline(BASELINE_PATH))
        current = set(localization_purity.build_baseline_keys(ROOT))
        self.assertEqual(stored, current, "baseline drifted from current findings")

    def test_current_repo_is_inside_baseline(self) -> None:
        # Stock violations are allowed; the gate only blocks NEW ones.
        localization_purity.check_visible_copy_purity(ROOT)

    def test_japanese_copy_is_complete(self) -> None:
        # Knife-3 completion gate: every entry except the exempt Auth_ContactAdmin
        # must carry a non-empty ja translation.
        missing = localization_purity.find_missing_japanese()
        self.assertEqual(
            missing,
            [],
            msg="ledger still has entries without Japanese copy",
        )

    def test_new_mixed_entry_is_caught_and_named(self) -> None:
        ledger = copy.deepcopy(localization_purity.load_ledger())
        ledger["entries"].append(
            {"id": "ZZ_Injected_Mix", "type": "ui", "zh": "描述 (Injected): 文本", "en": "Injected", "ja": "x"}
        )
        baseline = localization_purity.load_baseline(BASELINE_PATH)
        new = localization_purity.find_new_violations(
            localization_purity.find_ledger_mixes(ledger), baseline
        )
        self.assertIn("ZZ_Injected_Mix", {item["id"] for item in new})
        self.assertIn("ZZ_Injected_Mix", localization_purity.describe_violation(new[0]))

    def test_new_hardcoded_and_missing_japanese_are_caught_locatable(self) -> None:
        baseline = localization_purity.load_baseline(BASELINE_PATH)
        with tempfile.TemporaryDirectory() as temp_dir:
            src = Path(temp_dir) / "src"
            src.mkdir()
            (src / "ZZNewPanel.cpp").write_text(
                '#include "imgui.h"\nvoid Draw() { ImGui::Button("Brand New Panel Title"); }\n',
                encoding="utf-8",
            )
            findings = localization_purity.find_hardcoded_ui(src)
            self.assertTrue(findings)
            new = localization_purity.find_new_violations(findings, baseline)
            self.assertEqual(len(new), len(findings))
            located = localization_purity.describe_violation(new[0])
            self.assertIn("ZZNewPanel.cpp", located)
            self.assertIn("Brand New Panel Title", located)
            self.assertIn("Button", located)

        ledger = copy.deepcopy(localization_purity.load_ledger())
        ledger["entries"].append({"id": "ZZ_No_Ja", "type": "ui", "zh": "新条目", "en": "New entry"})
        new_ja = localization_purity.find_new_violations(
            localization_purity.find_missing_japanese(ledger), baseline
        )
        self.assertIn("ZZ_No_Ja", {item["id"] for item in new_ja})
        self.assertIn("缺日语", localization_purity.describe_violation(new_ja[0]))


    def test_batch4_word_package_and_boundary_fixes_are_clean(self) -> None:
        # Ratified whitelist ①(extensions): units adjacent to digits/CJK,
        # DAW-domain & vendor nouns, template placeholders, dotted paths.
        for text in (
            "发送到 DAW",
            "单击启用 (默认 80Hz)",
            "推荐时间：12ms",
            "仅WAV",
            "FFT大小",
            "通过 HRTF 在耳机上模拟",
            "前往 example.com 查看帮助",
            "支持 <字段名> 和 <字段名:N> 语法",
            "真峰值 [dBTP]",
            "数据库包: %s  WAL: %s  SHM: %s",
            "磁盘日志：%.2f MiB / %.0f MiB",
            "按 Esc 取消",
            "下一试听（历史前进）[Down]",
            "上一试听（历史回退）[Up]",
            "运行时也可按 F10 快速切换",
            "正在交接 Bootstrap",
            "Auro 数据库 (*.db)",
            "HTTP 主机桥接就绪，可直接发送。",
            "请启动主机，以便 HTTP 移交端点可用。",
            "需要排查 Bug 时可在这里打开",
            "清空迷你波形缓存",
            "语音识别用 Whisper Base 模型",
            "停止播放并返回开头 (PT: .)",
            "跳到结尾 (PT: Ctrl+Return)",
        ):
            self.assertFalse(localization_purity.is_language_mix(text), text)

    def test_batch4_package_does_not_swallow_prose_mix(self) -> None:
        # Bidirectional guard: common English nouns are NOT globally exempt,
        # and the ratified key-name words must not swallow longer words.
        for text in (
            "描述 (Description):",
            "重新分析 Key",
            "限制 UI 帧率为 60 FPS",
            "线程过多会吃满CPU并降低UI响应",
            "预览: OK=%d",
            "清空 Detail 波形",
            "点击 Upgrade 按钮继续",
            "请通过 AuroTools 导入",
            "bootstrapping 失败",
            "HTTPS 端口未开放",
            "SHMdisk 异常",
            "测量 Mibcap 值",
            "调用 BootstrapMgr 注册",
            "按 Power 键重启",
            "解释 PTX 格式",
            "按下 ReturnKey 重试",
        ):
            self.assertTrue(localization_purity.is_language_mix(text), text)
        # pure-English lines are never a "mix" regardless of words present
        self.assertFalse(localization_purity.is_language_mix("Playback history forward"))

    def test_batch4_row_registry_is_token_limited(self) -> None:
        def mixes(entries):
            return {
                item["id"]
                for item in localization_purity.find_ledger_mixes({"entries": entries})
            }
        registry_row = {
            "id": "Search_SyntaxAND",
            "type": "ui",
            "zh": "explosion fire      → 同时含两词 (AND)",
            "en": "explosion fire -> both words (AND)",
            "ja": "x",
        }
        self.assertNotIn("Search_SyntaxAND", mixes([registry_row]))
        smuggled = dict(registry_row, zh="explosion fire 混入 Foo 同时含两词")
        self.assertIn("Search_SyntaxAND", mixes([smuggled]))
        unlisted = {"id": "ZZ_Random", "type": "ui", "zh": "切换到 Camelot 模式", "en": "x", "ja": "y"}
        self.assertNotIn("ZZ_Random", mixes([unlisted]))  # Camelot is a global noun
        prose = {"id": "ZZ_Random2", "type": "ui", "zh": "重新分析 Key", "en": "x", "ja": "y"}
        self.assertIn("ZZ_Random2", mixes([prose]))
        # 7-A tier-3: key-bound tokens (Reverse/Mono/Tap) pass ONLY on their
        # registered keys; the same wording smuggled into another id stays red.
        bound = {"id": "Sc_ToggleReverse", "type": "ui", "zh": "倒放 (Reverse)", "en": "x", "ja": "y"}
        self.assertNotIn("Sc_ToggleReverse", mixes([bound]))
        smuggled = dict(bound, id="ZZ_Smuggled", zh="倒放 (Reverse)")
        self.assertIn("ZZ_Smuggled", mixes([smuggled]))
        # 9b row-bound tokens: mini/px pass only on their registered keys
        mini = {"id": "Wf_StageMiniWave", "type": "ui", "zh": "正在缓存 mini 波形", "en": "x", "ja": "y"}
        self.assertNotIn("Wf_StageMiniWave", mixes([mini]))
        px = {"id": "CoverResLow", "type": "ui", "zh": "128px (极低显存)", "en": "x", "ja": "y"}
        self.assertNotIn("CoverResLow", mixes([px]))
        self.assertIn("ZZ_PX", mixes([dict(px, id="ZZ_PX")]))
        self.assertIn("ZZ_MINI", mixes([dict(mini, id="ZZ_MINI")]))
        scan = {"id": "Scan_FullModeHint", "type": "ui", "zh": "完整模式：metadata、封面和 mini 波形都进入本次入库完成门。", "en": "x", "ja": "y"}
        self.assertNotIn("Scan_FullModeHint", mixes([scan]))
        self.assertIn("ZZ_SCAN", mixes([dict(scan, id="ZZ_SCAN")]))
        tips = {"id": "Search_NoResultTips", "type": "ui", "zh": "可尝试更短关键词、拼音首字母、path:路径片段，或清除筛选条件。", "en": "x", "ja": "y"}
        self.assertNotIn("Search_NoResultTips", mixes([tips]))
        self.assertIn("ZZ_TIPS", mixes([dict(tips, id="ZZ_TIPS")]))

    def test_about_brand_names_are_allowed_only_in_their_own_rows(self) -> None:
        license_row = {
            "id": "Main_AboutLicenseNote", "type": "ui",
            "zh": "本软件基于 Apache License 2.0 开源发布",
            "en": "Released under the Apache License 2.0", "ja": "x",
        }
        credit_row = {
            "id": "Main_AboutCindyCredit", "type": "ui",
            "zh": "感谢 Cindy 对本软件的开发支持。",
            "en": "With thanks to Cindy for development support of this software.", "ja": "x",
        }
        def mixed_ids(*rows):
            return {item["id"] for item in localization_purity.find_ledger_mixes({"entries": rows})}

        self.assertEqual(mixed_ids(license_row, credit_row), set())
        self.assertEqual(mixed_ids(dict(license_row, zh="本软件基于 Apache License extra 开源发布")),
                         {"Main_AboutLicenseNote"})
        self.assertEqual(mixed_ids(dict(credit_row, zh="感谢 Cindy extra 对本软件的开发支持。")),
                         {"Main_AboutCindyCredit"})
        self.assertEqual(mixed_ids(dict(credit_row, id="ZZ_UnrelatedCredit")), {"ZZ_UnrelatedCredit"})

    def test_batch4_scans_colored_and_new_controls(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            src = Path(temp_dir) / "src"
            src.mkdir()
            (src / "ZZBatch4Controls.cpp").write_text(
                "\n".join(
                    [
                        "#include \"imgui.h\"",
                        "void Draw() {",
                        "    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f), \"库缺失中\");",
                        "    ImGui::Text(\"移动: %s\", x);",
                        "    ImGui::SeparatorText(u8\"分隔标题\");",
                        "    ImGui::ProgressBar(frac, size, u8\"传输进度\");",
                        "    ImGui::TextColored(col, L(LStr::Main_CancelBtn4));",
                        "    ImGui::Text(\"%d\", v);",
                        "}",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            texts = {item["text"] for item in localization_purity.find_hardcoded_ui(src)}
            self.assertIn("库缺失中", texts)
            self.assertIn("移动: %s", texts)
            self.assertIn("分隔标题", texts)
            self.assertIn("传输进度", texts)
            self.assertNotIn("%d", texts)
            self.assertTrue(all("Main_CancelBtn4" not in t for t in texts))

    def test_batch4_mojibake_is_hard_gate(self) -> None:
        arrow = "â" + chr(0x86) + chr(0x92)
        ledger = {"entries": [{"id": "ZZ_Moji", "type": "ui", "zh": "正常", "en": f"both {arrow} words", "ja": "x"}]}
        hits = localization_purity.find_mojibake(ledger)
        self.assertEqual([item["id"] for item in hits], ["ZZ_Moji"])
        self.assertIn("ZZ_Moji", localization_purity.describe_violation(hits[0]))
        # never baselined, and real ledger is clean
        self.assertTrue(all(not k.startswith("mojibake|") for k in localization_purity.build_baseline_keys()))
        self.assertEqual(localization_purity.find_mojibake(), [])

    def test_f1_registered_helpers_bidirectional(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            src = Path(temp_dir) / "src"
            src.mkdir()
            (src / "main.cpp").write_text(
                "\n".join([
                    "#include \"imgui.h\"",
                    "static void SectionHeader(const char* a, const char* b=nullptr) {}",
                    "void DrawA() {",
                    "    SectionHeader(u8\"外观与主题\", u8\"选择主题\");",           # 2 hits
                    "    SectionHeader(L(Set_OK), nullptr);",                        # 0 hits
                    "    auto row = [&](const char* k, int v) { };",
                    "    row(\"BitDepth\", v);",                                     # 1 hit
                    "    rowInt(\"Channels\", v);",                                  # 1 hit
                    "}",
                    "static std::string GetCapabilityTag(int i) {",
                    "    return u8\"Running\";",                                     # registered fn -> hit
                    "}",
                    "static const char* SomeOtherBuilder(int i) {",
                    "    return u8\"未登记的函数不该被抓\";",                          # NOT registered -> 0
                    "}",
                    "void FillIndex() {",
                    "    addEntry(0, u8\"外观与主题\", u8\"外观 主题 theme appearance\");",  # title slot hit
                    "    pushSectionMatch(0, L(Settings_SectionDisplay), u8\"显示 display\");",  # L() -> 0
                    "}",
                ],
                ) + "\n",
                encoding="utf-8",
            )
            (src / "OtherFile.cpp").write_text(
                "\n".join([
                    "#include \"imgui.h\"",
                    "void DrawB() {",
                    "    row(\"NoLambdaDef\", v);",                                  # file lacks `auto row =` -> 0
                    "    static std::string GetCapabilityTag(int i) { return u8\"x\"; }",  # wrong file -> 0
                    "}",
                ],
                ) + "\n",
                encoding="utf-8",
            )
            texts = [(x["func"], x["text"]) for x in localization_purity.find_hardcoded_ui(src)]
            self.assertIn(("SectionHeader", "外观与主题"), texts)
            self.assertIn(("SectionHeader", "选择主题"), texts)
            self.assertIn(("row", "BitDepth"), texts)
            self.assertIn(("rowInt", "Channels"), texts)
            self.assertIn(("GetCapabilityTag", "Running"), texts)
            self.assertNotIn(("SomeOtherBuilder", "未登记的函数不该被抓"), texts)
            self.assertFalse(any(f == "row" and t == "NoLambdaDef" for f, t in texts))
            self.assertFalse(any(t == "Set_OK" for _, t in texts))
            self.assertIn(("addEntry", "外观与主题"), texts)
            self.assertFalse(any(t.startswith("外观 主题 theme") for _, t in texts))
            self.assertEqual(sum(1 for f, _ in texts if f == "addEntry"), 1)
            self.assertEqual(len([f for f, _ in texts if f == "SectionHeader"]), 2)


    def test_w61_win32_sites_are_exactly_named(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            src = Path(temp_dir) / "src"
            (src / "platform").mkdir(parents=True)
            (src / "WindowsMain.cpp").write_text(
                "\n".join([
                    "void F() {",
                    "    ShowMessageBox(hwnd, L\"提示正文\", L\"框标题\", MB_OK);",       # 2 hits
                    "    wcscpy_s(nid.szTip, L\"托盘提示\");",                          # 1 hit
                    "    AppendMenuW(hMenu, MF_STRING, 2, L\"隐藏窗口\");",             # 1 hit
                    "    SetDlgItemTextW(dlg, 3, L\"未登记 API 不该被抓\");",           # 0
                    "    auto x = L\"漂浮字面量不在站点点名内\";",                      # 0
                    "}",
                ],) + "\n", encoding="utf-8",
            )
            (src / "platform" / "audio_file_dialog_win32.cpp").write_text(
                'void G() { ofn.lpstrFilter = L"WAV Filter Name"; }\n', encoding="utf-8",
            )
            texts = {x["text"] for x in localization_purity.find_hardcoded_ui(src)
                     if x["func"].startswith("Win32:")}
            self.assertIn("提示正文", texts)
            self.assertIn("框标题", texts)
            self.assertIn("托盘提示", texts)
            self.assertIn("隐藏窗口", texts)
            self.assertIn("WAV Filter Name", texts)   # assign-anchor captures multi-token stmt
            self.assertNotIn("未登记 API 不该被抓", texts)
            self.assertNotIn("漂浮字面量不在站点点名内", texts)


    def test_knife70_wrapper_secondslot_and_escape_anchors(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            src = Path(temp_dir) / "src"
            src.mkdir()
            (src / "ZZF6Panel.cpp").write_text(
                "\n".join([
                    "#include \"imgui.h\"",
                    "void Draw() {",
                    "    BeginTransientScrollbarWindow(u8\"合成窗口题名\", &open);",
                    "    BeginTransientScrollbarPopupModal(u8\"合成弹窗题名##Fx\", nullptr, 0);",
                    "    DrawUnifiedSearchInput(\"##Search\", u8\"合成占位提示...\", &buf);",
                    "    SomeUnregisteredWindow(u8\"未登记包装不该被抓\", &x);",
                    "    // BeginTransientScrollbarWindow(u8\"注释里的包装调用不该被抓\");",
                    "    ImGui::TableSetupColumn(u8\"合成列头\", 0, 100.0f);",
                    "    ImGui::InputInt(u8\"合成整数标签\", &v);",
                    "    ImGui::Text(L(Settings_OK), names.empty() ? u8\"无\" : names.c_str());",
                    "    ImGui::SmallButton(camelot ? \"合成英字\" : u8\"合成调名\");",
                    "    ImGui::Text(u8\"已有首位\", s ? u8\"参数位逃逸\" : nullptr);",
                    "    ImGui::Button(u8\"\\u5408\\u6210\\u8f6c\\u4e49\");",
                    "    HintAwareInputTextWithHint(\"##FxSearch\", u8\"合成浏览占位…\", buf, size);",
                    "    HintAwareInputTextWithHint(\"##FxKey\", \"sk-...\", buf, size);",
                    "}",
                ]) + "\n",
                encoding="utf-8",
            )
            texts = {f["text"] for f in localization_purity.find_hardcoded_ui(src)}
            funcs = {f["func"] for f in localization_purity.find_hardcoded_ui(src)}
            self.assertIn("合成窗口题名", texts)
            self.assertIn("合成弹窗题名##Fx", texts)
            self.assertIn("合成占位提示...", texts)
            self.assertIn("F6:BeginTransientScrollbarWindow", funcs)
            self.assertIn("F6:DrawUnifiedSearchInput", funcs)
            self.assertNotIn("未登记包装不该被抓", texts)
            self.assertNotIn("注释里的包装调用不该被抓", texts)
            self.assertIn("合成列头", texts)          # TableSetupColumn arg0
            self.assertIn("合成整数标签", texts)       # InputInt arg0
            self.assertIn("无", texts)               # arg-slot ternary, single CJK kept now
            self.assertIn("合成英字", texts)     # ternary inside arg0 of a bare control
            self.assertIn("合成调名", texts)
            self.assertIn("已有首位", texts)           # primary path untouched
            self.assertNotIn("参数位逃逸", texts)      # no secondary when arg0 is a literal
            self.assertIn("合成转义", texts)           # \uXXXX decode pass
            self.assertIn("合成浏览占位…", texts)      # HintAware wrapper (7-A slice-3 anchor)
            self.assertNotIn("sk-...", texts)            # technical key-format example kept out of the debt ledger
            self.assertNotIn("\\u5408\\u6210\\u8f6c\\u4e49", texts)

    def test_harken_tooltip_wrapper_has_f6_anchor_and_keeps_aura_compatibility(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            src = Path(temp_dir) / "src"
            src.mkdir()
            (src / "ZZTooltip.cpp").write_text(
                "\n".join([
                    "void Draw() {",
                    '    HarkenSearch::SetTooltip(u8"合成新提示");',
                    '    HarkenSearch::SetTooltip(L(Existing_Tip));',
                    '    Aura::SetTooltip(u8"合成旧提示");',
                    "}",
                ]) + "\n",
                encoding="utf-8",
            )
            hits = {(item["func"], item["text"])
                    for item in localization_purity.find_hardcoded_ui(src)
                    if item["func"].startswith("F6:")}
            self.assertIn(("F6:HarkenSearch::SetTooltip", "合成新提示"), hits)
            self.assertIn(("F6:Aura::SetTooltip", "合成旧提示"), hits)
            self.assertNotIn(("F6:HarkenSearch::SetTooltip", "Existing_Tip"), hits)

    def test_knife9c_visible_keep_set_is_exact(self) -> None:
        # Lead-ratified exempt set: exact strings vanish, lookalikes stay red.
        with tempfile.TemporaryDirectory() as temp_dir:
            src = Path(temp_dir) / "src"
            src.mkdir()
            (src / "ZZKeep.cpp").write_text(
                "\n".join([
                    "#include \"imgui.h\"",
                    "void Draw() {",
                    "    ImGui::TableSetupColumn(\"LUFS\", flags, 64);",
                    "    ImGui::SmallButton(\"Camelot\");",
                    "    ImGui::SetTooltip(\"%s\", \"OFF\");",
                    "    ImGui::TableSetupColumn(\"LUFSX\", flags, 64);",
                    "    ImGui::SmallButton(\"Camelot2\");",
                    "    ImGui::SetTooltip(\"%s\", \"OFF-line\");",
                    "}",
                ]) + "\n",
                encoding="utf-8",
            )
            texts = {f["text"] for f in localization_purity.find_hardcoded_ui(src)}
            for kept in ("LUFS", "Camelot", "OFF"):
                self.assertNotIn(kept, texts)
            for red in ("LUFSX", "Camelot2", "OFF-line"):
                self.assertIn(red, texts)

    def test_knife70_registry_tables_and_vars(self) -> None:
        masked = "\n".join([
            "static ShortcutEntry fxRows[] = {",
            "    { A, u8\"合成行名\", u8\"合成行说明\", { K } },  // { Z, u8\"注释不算\", u8\"也不算\" }",
            "};",
            "const char* fxLabel = state.err.empty() ? u8\"合成标签\" : L(Feedback_Retry);",
            "const char* fwdLabel2 = \"Continue\";",
            "const char* unlisted[] = { u8\"没登记的表\" };",
            "",
        ])
        hits = localization_purity._f6_registry_findings(
            "Fixture.cpp", masked,
            tables={("Fixture.cpp", "fxRows")},
            vars={("Fixture.cpp", "fxLabel"), ("Fixture.cpp", "fwdLabel2")},
        )
        texts = {h["text"] for h in hits}
        self.assertIn("合成行名", texts)
        self.assertIn("合成行说明", texts)
        self.assertIn("合成标签", texts)
        self.assertIn("Continue", texts)     # var registry catches English feeders too
        self.assertNotIn("注释不算", texts)   # comment-masked
        self.assertNotIn("没登记的表", texts)  # unregistered symbol is out of scope
        self.assertIn("没登记的表", {h["text"] for h in localization_purity._f6_registry_findings(
            "Fixture.cpp", masked, tables={("Fixture.cpp", "unlisted")}, vars=set())})

    def test_knife70_return_helper_ternary(self) -> None:
        old = localization_purity.RETURN_HELPER_FNS
        localization_purity.RETURN_HELPER_FNS = old | {("ZZGreet.cpp", "TimeOfDayPhrase")}
        try:
            with tempfile.TemporaryDirectory() as temp_dir:
                src = Path(temp_dir) / "src"
                src.mkdir()
                (src / "ZZGreet.cpp").write_text(
                    "\n".join([
                        "std::string TimeOfDayPhrase(int hour, Lang lang) {",
                        "    if (hour < 12) return (lang == Lang::Chinese) ? u8\"合成早上好\" : \"Good morning\";",
                        "    return s;",
                        "}",
                    ]) + "\n",
                    encoding="utf-8",
                )
                texts = {f["text"] for f in localization_purity.find_hardcoded_ui(src)}
                self.assertIn("合成早上好", texts)
                self.assertIn("Good morning", texts)
        finally:
            localization_purity.RETURN_HELPER_FNS = old

    def test_knife70_single_cjk_is_visible_copy(self) -> None:
        self.assertTrue(localization_purity._keep_visible_literal("Text", "无"))
        self.assertFalse(localization_purity._keep_visible_literal("Text", "%s"))
        self.assertFalse(localization_purity._keep_visible_literal("Text", "x"))
        self.assertFalse(localization_purity._keep_visible_literal("Text", "×"))

    def test_all_ledger_fmt_conversion_sets_match(self) -> None:
        """10-A (RQ-20260911-04): every ledger entry must carry the SAME
        conversion specifiers IN THE SAME ORDER in zh/en/ja. T1 (Greptile
        PR#175 P2): upgraded from set-equality to ORDER equality — MSVC has
        no positional %1$s, a transposed slot silently mis-types at render."""
        import json

        root = Path(__file__).resolve().parents[1]
        ledger = json.loads((root / "locale" / "harken.ui.strings.json").read_text(encoding="utf-8"))
        fmt = localization_purity.FORMAT_RE
        for entry in ledger["entries"]:
            zh = fmt.findall(entry["zh"])
            en = fmt.findall(entry.get("en", ""))
            ja = fmt.findall(entry.get("ja", ""))
            self.assertEqual(zh, en, f"{entry['id']}: zh/en conversion ORDER mismatch")
            self.assertEqual(zh, ja, f"{entry['id']}: zh/ja conversion ORDER mismatch")

    # ---- T1 (RQ-20260911-04): tech-keep bidirectional + private-table gate ----

    def _synth_findings(self, fname: str, body: str) -> set[str]:
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "src"
            src.mkdir()
            (src / fname).write_text(body, encoding="utf-8")
            return {f["text"] for f in localization_purity.find_hardcoded_ui(src)}

    def test_tech_keep_exact_members_exempt(self) -> None:
        # direction 1: registered exact literals never surface as findings
        texts = self._synth_findings(
            "ZZKeep.cpp",
            'void f() {\n    ImGui::TableSetupColumn("CatID", 0);\n'
            '    ImGui::TextUnformatted("sk-...");\n}\n')
        self.assertNotIn("CatID", texts)
        self.assertNotIn("sk-...", texts)

    def test_tech_keep_lookalikes_flagged(self) -> None:
        # direction 2: look-alikes must NOT ride the exemption set (both families)
        texts = self._synth_findings(
            "ZZKeep.cpp",
            'void f() {\n    ImGui::TableSetupColumn("CatID2", 0);\n'
            '    ImGui::TableSetupColumn("Cat ID", 0);\n'
            '    ImGui::TableSetupColumn("CATEGORY ID", 0);\n'
            '    ImGui::TableSetupColumn("SK-...", 0);\n'
            '    ImGui::TableSetupColumn("sk-..", 0);\n}\n')
        self.assertEqual({"CatID2", "Cat ID", "CATEGORY ID", "SK-...", "sk-.."}, texts)

    def test_fmt_order_helper_flags_transposition(self) -> None:
        # set-equal but order-transposed = MSVC mis-type storm; must fail
        self.assertFalse(localization_purity.fmt_order_match(
            "已添加 %d 个到 %s", "Added %s files to %d", "%s に %d 個"))

    def test_fmt_order_helper_accepts_aligned(self) -> None:
        self.assertTrue(localization_purity.fmt_order_match(
            "已添加 %d 个文件到 %s", "Added %d files to %s", "%d 個のファイルを追加：%s"))

    def _synth_private_debt(self, fname: str, body: str) -> list[dict]:
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "src"
            src.mkdir()
            (src / fname).write_text(body, encoding="utf-8")
            hits = localization_purity.find_private_tables(src)
            return [h for h in hits if not h["registered"]]

    def test_private_table_unregistered_shape_flags(self) -> None:
        # a NEW file smuggling a {en, zh} private table must be debt, not silence
        debt = self._synth_private_debt(
            "ZZNewPanel.cpp",
            'static const struct { const char* en; const char* zh; } kT[] = {\n'
            '    {"Connect", u8"连接"},\n};\n')
        self.assertEqual([("ZZNewPanel.cpp", "Connect", "连接")],
                         [(d["path"].split("/")[-1], d["ascii"], d["cjk"]) for d in debt])

    def test_private_table_registered_file_new_pair_still_debt(self) -> None:
        # pair-granularity: a registered FILE must not swallow NEW pairs in it
        debt = self._synth_private_debt(
            "TagSystem.cpp",
            'void f() { addTag("GhostEn", u8"幽灵牌", TagDimension::Mood); }\n')
        self.assertEqual([("GhostEn", "幽灵牌")],
                         [(d["ascii"], d["cjk"]) for d in debt])
        # exact registered pair inside the same file: exempt
        exempt = self._synth_private_debt(
            "TagSystem.cpp",
            'void f() { addTag("Ambient", u8"氛围", TagDimension::Category); }\n')
        self.assertEqual([], exempt)

    def test_private_table_registry_key_ignores_ancestor_src_segments(self) -> None:
        # checkout paths with extra src/ must still key as repo-relative src/TagSystem.cpp
        with tempfile.TemporaryDirectory() as td:
            nested = Path(td) / "src" / "aris" / "src"
            nested.mkdir(parents=True)
            (nested / "TagSystem.cpp").write_text(
                'void f() { addTag("Ambient", u8"氛围", TagDimension::Category); }\n',
                encoding="utf-8")
            hits = localization_purity.find_private_tables(nested)
            self.assertEqual(["src/TagSystem.cpp"], [h["path"] for h in hits])
            self.assertTrue(hits)
            self.assertTrue(all(h["registered"] for h in hits))

    def test_private_table_multiline_adjacent_literals_are_found(self) -> None:
        debt = self._synth_private_debt(
            "ZZWrap.cpp",
            'static const struct { const char* en; const char* zh; } kT[] = {\n'
            '    {"Connect",\n'
            '     u8"连接"},\n};\n')
        self.assertEqual([("ZZWrap.cpp", "Connect", "连接")],
                         [(d["path"].split("/")[-1], d["ascii"], d["cjk"]) for d in debt])

    def test_private_table_repo_debt_census_exact(self) -> None:
        """Handover register mirror (docs/process/RQ-20260909-02-purity-inventory.md):
        exactly these 42 unregistered pairs exist today; any drift (new pair,
        renamed pair, migration) turns this test red on purpose so the board
        re-ratifies. 归属: TI区名 29 → ThemeInspector 候裁, QA 假数据 2 + 更新带
        2 → ⑧挂起带, AudioEngine 6 → 10-F/G, Doctor 3 → 10-H."""
        debt = [h for h in localization_purity.find_private_tables() if not h["registered"]]
        actual = sorted((h["path"], h["ascii"], h["cjk"]) for h in debt)
        expected = sorted([
            ("src/AudioEngine.h", "ATSC A/85 (US TV)", "ATSC A/85（美国电视）"),
            ("src/AudioEngine.h", "Cinema (SMPTE RP 200)", "电影 (SMPTE RP 200)"),
            ("src/AudioEngine.h", "EBU R128 (Broadcast)", "EBU R128（广播）"),
            ("src/AudioEngine.h", "Game Audio", "游戏音频"),
            ("src/AudioEngine.h", "Podcast / Audiobook", "播客/有声书"),
            ("src/AudioEngine.h", "Streaming (Spotify/YT)", "流媒体 (Spotify/YT)"),
                                                                                                                                    ("src/main.cpp", "fix", "更新中心"),
            ("src/main.cpp", "fix", "安装"),
                                                                                                                                                                                                                                                                                    ("src/update/HarkenSearchUpdateNoticeUi.cpp", "history", "更新历史"),
            ("src/update/HarkenSearchUpdateNoticeUi.cpp", "history-dismiss", "知道了"),
        ])
        self.assertEqual(expected, actual)

    def test_live_baseline_channels_isolated(self) -> None:
        # the private-table channel must NOT pollute live/baseline numbers
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "src"
            src.mkdir()
            (src / "ZZPriv.cpp").write_text(
                'const char* kT[] = { "Alpha", u8"甲", "Beta", u8"乙", nullptr };\n', encoding="utf-8")
            self.assertEqual([], localization_purity.find_hardcoded_ui(src))
            self.assertEqual(2, len(localization_purity.find_private_tables(src)))

    # ---- 10-QA1: splash-helper and menu-label-var shapes are now gated ----

    def test_splash_local_helpers_flag(self) -> None:
        # direction red: injecting CJK into the startup-progress helpers must trip
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "src"
            src.mkdir()
            (src / "WindowsMain.cpp").write_text(
                'void f() {\n'
                '    auto presentProgress = [&](int a, int b, const std::string& m) {};\n'
                '    presentProgress(0, 1, u8"注入步骤文案");\n'
                '    auto renderBetaSplashFrame = [&](const std::string& m) {};\n'
                '    renderBetaSplashFrame(u8"注入预热文案");\n}\n', encoding="utf-8")
            texts = {f["text"] for f in localization_purity.find_hardcoded_ui(src)}
            self.assertIn("注入步骤文案", texts)
            self.assertIn("注入预热文案", texts)

    def test_splash_helper_ledger_arg_passes(self) -> None:
        # direction green: migrated call sites (L(id), no literal) stay clean
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "src"
            src.mkdir()
            (src / "WindowsMain.cpp").write_text(
                'void f() {\n'
                '    auto presentProgress = [&](int a, int b, const std::string& m) {};\n'
                '    presentProgress(0, 1, L(Splash_PrepareCoverList));\n}\n', encoding="utf-8")
            self.assertEqual([], localization_purity.find_hardcoded_ui(src))

    def test_menu_label_vars_flag(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "src"
            src.mkdir()
            (src / "main.cpp").write_text(
                'void f() {\n'
                '    const char* playlistQuickBarMenuLabel = u8"注入隐藏项";\n'
                '    constexpr const char* kPopupTitle = u8"注入标题";\n'
                '    ImGui::MenuItem(playlistQuickBarMenuLabel);\n}\n', encoding="utf-8")
            texts = {f["text"] for f in localization_purity.find_hardcoded_ui(src)}
            self.assertIn("注入隐藏项", texts)
            self.assertIn("注入标题", texts)

    def test_repo_qa1_sites_migrated_clean(self) -> None:
        fs = localization_purity.find_hardcoded_ui()
        funcs = {f.get("func", "") for f in fs}
        self.assertNotIn("F6var:playlistQuickBarMenuLabel", funcs)
        self.assertNotIn("F6var:kPopupTitle", funcs)
        self.assertNotIn("F6:presentProgress", funcs)
        self.assertNotIn("F6:renderBetaSplashFrame", funcs)

    def test_popup_title_shape_flags_format_slot_never(self) -> None:
        # E1B: PopupTitle label slot is gated; pure printf-format slots (the reason
        # PopupMuted is NOT registered) must never be flagged as visible copy.
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "src"
            src.mkdir()
            (src / "main.cpp").write_text(
                'void f() {\n'
                '    PopupTitle(u8"注入标题");\n'
                '    PopupTitle(L(TabPresets_Title));\n'
                '    PopupMuted(" %s", q);\n'
                '}\n', encoding="utf-8")
            fs = localization_purity.find_hardcoded_ui(src)
            texts = {x["text"] for x in fs}
            self.assertIn("注入标题", texts)
            self.assertNotIn(" %s", texts)
            self.assertNotIn("TabPresets_Title", texts)

    # ---- gate-ext ticket (RQ-20260911-04): three feeder shapes, both directions ----

    def test_utp_progress_shape_flags_both_ways(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "src"
            src.mkdir()
            (src / "AIEngine.cpp").write_text(
                'void f(Task& t) {\n'
                '    UpdateTaskProgress(t.id, 0.1f, u8"注入进度语");\n'
                '    UpdateTaskProgress(t.id, 0.2f, L(AI_Prog_Ok));\n'
                '    UpdateTaskProgress(t.id, 0.3f, std::string("ascii ok"));\n}\n',
                encoding="utf-8")
            texts = {x["text"] for x in localization_purity.find_hardcoded_ui(src)}
            self.assertIn("注入进度语", texts)
            self.assertNotIn("AI_Prog_Ok", texts)
            self.assertNotIn("ascii ok", texts)

    def test_snprintf_display_buf_shape_flags_both_ways(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "src"
            src.mkdir()
            (src / "main.cpp").write_text(
                'void f() {\n'
                '    char chLabelBuf[64];\n'
                '    snprintf(chLabelBuf, sizeof(chLabelBuf),\n'
                '        u8"注入声道形 %d", n);\n'
                '    char otherBuf[64];\n'
                '    snprintf(otherBuf, sizeof(otherBuf), u8"未注册缓冲不报");\n'
                '    snprintf(chLabelBuf, sizeof(chLabelBuf), "%d", n);\n}\n',
                encoding="utf-8")
            fs = localization_purity.find_hardcoded_ui(src)
            funcs = {x.get("func", "") for x in fs}
            texts = {x["text"] for x in fs}
            self.assertIn("F6snprintf:chLabelBuf", funcs)
            self.assertIn("注入声道形 %d", texts)
            self.assertNotIn("未注册缓冲不报", texts)
            self.assertNotIn("%d", texts)

    def test_json_payload_shape_flags_both_ways(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "src"
            src.mkdir()
            (src / "WindowsMain.cpp").write_text(
                'void f() {\n'
                '    payload += JsonEscape(std::string(u8"注入载荷"));\n'
                '    payload += JsonEscape(std::string(L(Done)));\n}\n',
                encoding="utf-8")
            fs = localization_purity.find_hardcoded_ui(src)
            funcs = {x.get("func", "") for x in fs}
            self.assertIn("F6payload:JsonEscape", funcs)
            self.assertIn("注入载荷", {x["text"] for x in fs})

    # ---- RQ-20260911-01: baseline growth must not hide behind an edited JSON ----

    COMMITTED_STOCK_KEYS = (
        'mix|Consent_EulaCaption|zh',
        'mix|Consent_PrivacyCaption|zh',
        'mix|DBflow_BundledVerifyFail|zh',
        'mix|DBflow_InstalledVerified|zh',
        'mix|DBflow_MkdirFail|zh',
        'mix|DBflow_StagedVerifyFail|zh',
        'mix|DBflow_UserPluginsUnknown|zh',
        'mix|DBflow_UserPluginsUnlocatable|zh',
        'mix|DBst_ExeMissR|zh',
        'mix|DBst_StaleDirD|zh',
        'mix|Db_DeleteLockedFmt|zh',
        'mix|Db_Fts5DisabledFmt|zh',
        'mix|Db_FtsRebuildFailFmt|zh',
        'mix|Db_FtsVocabFailFmt|zh',
        'mix|Db_TrigramCreateFailFmt|zh',
        'mix|Db_TrigramRebuildFailFmt|zh',
        'mix|Db_TrigramTriggerFailFmt|zh',
        'mix|Db_TrigramUpgradeFailFmt|zh',
        'mix|DockTest_Band|zh',
        'mix|ThemeCol_historyCapsuleActiveBG|zh',
        'mix|ThemeCol_historyCapsuleBG|zh',
        'mix|ThemeCol_miniWaveBG|zh',
        'mix|ThemeCol_miniWaveColor|zh',
        'mix|Tools_DockTestWindow|zh',
        'mix|Tools_ResetDockTestLayout|zh',
        'ui|DevToolsHarkenSearchSmoke.cpp|Begin|191596e961ca1d4b',
        'ui|DevToolsHarkenSearchSmoke.cpp|BulletText|3edc5b816445db49',
        'ui|DevToolsHarkenSearchSmoke.cpp|BulletText|54a37b9545bf9109',
        'ui|DevToolsHarkenSearchSmoke.cpp|BulletText|6ca2dcc28cf841ae',
        'ui|DevToolsHarkenSearchSmoke.cpp|BulletText|85b8f180bf4210df',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|0c0bcfc20ce57259',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|15ab3c08f1b1dabb',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|2f182eb52dc180ce',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|4c4ca97f245bb773',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|53d4c19c0e5e61e8',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|663f15383e879e20',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|754df615ccdac996',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|826acf0a64cac75f',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|9734eabbfa529adf',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|9bcf5311040d4a05',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|a94a7b0f713a9f00',
        'ui|DevToolsHarkenSearchSmoke.cpp|Button|c8ce41ed65f3eb48',
        'ui|DevToolsHarkenSearchSmoke.cpp|Checkbox|f2cf0b56564e8226',
        'ui|DevToolsHarkenSearchSmoke.cpp|Combo|0eef700775968615',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|0557456e97a966d4',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|15330a13bc85b94b',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|2a06dd8efed1a1d8',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|30f41464060ff92f',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|38cb35d40ad1ecef',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|69b21539ab317e25',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|6b2c3995da797cb5',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|79f1f90c73098e1c',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|7b060e5ee21489ff',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|7e13938f699006d6',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|8beaecf42a77307d',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|8f86479b8d9c2444',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|9e606ef2a712c1dd',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|a51a7fc6659fe766',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|b17eb4ca0f4e9763',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|b6ed3e93a1701fcd',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|bbdc87a2dbe733ce',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|d55ee60d248788f1',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|dfc2817df6cb1fcb',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|e4d7d36eb0e5a927',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|e6703abec59fd070',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|ec7bfe15a1c83811',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|fa21abe314e5a5e0',
        'ui|DevToolsHarkenSearchSmoke.cpp|TextUnformatted|fc9232d0df2eb2e2',
        'ui|HarkenSearchWidgets.cpp|TextUnformatted|1658150c0a9bc71f',
        'ui|HarkenSearchWidgets.cpp|TextUnformatted|55bfe891374f20c3',
        'ui|WindowsMain.cpp|Win32:ShowMessageBox|1aad79fe8f0b2376',
        'ui|main.cpp|Button|fd94f4f12897c3d7',
        'ui|main.cpp|MenuItem|01ce503ec742061f',
        'ui|main.cpp|TextDisabled|1b4eb7735d4b8652',
        'ui|main.cpp|TextDisabled|2d9f5eb22609af95',
        'ui|main.cpp|TextDisabled|9c708e719999f8a2',
        'ui|main.cpp|TextDisabled|f6812c08ed261ebe',
        'ui|main.cpp|TextDisabled|f91f0ec55e45f478',
        'ui|main.cpp|TextUnformatted|637a06fc034c2647',
        'ui|main.cpp|TextWrapped|a28ebeb31880322d',
        'ui|update/HarkenSearchUpdateNoticeUi.cpp|TableSetupColumn|73214a24e9dc6de4',
        'ui|update/HarkenSearchUpdateNoticeUi.cpp|TableSetupColumn|96e13b098a64396a',
    )

    def _growth_payload(
        self,
        keys: list[str],
        *,
        cap: int = 32,
        exemptions: dict | None = None,
        schema: int = 2,
        stock_keys: list[str] | None = None,
    ) -> dict:
        stock = stock_keys if stock_keys is not None else [f"mix|Stock{i:02d}|zh" for i in range(32)]
        return {
            "schema": schema,
            "cap": cap,
            "stockKeys": stock,
            "keys": keys,
            "growthExemptions": {} if exemptions is None else exemptions,
        }

    def _stock_keys(self, extra: list[str] | None = None) -> list[str]:
        keys = [f"mix|Stock{i:02d}|zh" for i in range(32)]
        if extra:
            keys = keys + extra
        return keys

    def test_unexempted_baseline_growth_fails_and_names_extra_keys(self) -> None:
        payload = self._growth_payload(self._stock_keys(["mix|ZZ_Sneak|zh"]))
        with self.assertRaises(localization_purity.PurityError) as ctx:
            localization_purity.check_baseline_growth(payload)
        self.assertIn("mix|ZZ_Sneak|zh", str(ctx.exception))

    def test_exempting_stock_key_cannot_hide_unexempted_extra(self) -> None:
        stock = self._stock_keys()
        payload = self._growth_payload(
            stock + ["mix|ZZ_Sneak|zh"],
            exemptions={
                stock[0]: {
                    "owner": "RQ-20260999-01",
                    "reason": "trying to free a slot",
                }
            },
        )
        with self.assertRaises(localization_purity.PurityError) as ctx:
            localization_purity.check_baseline_growth(payload)
        self.assertIn("mix|ZZ_Sneak|zh", str(ctx.exception))

    def test_exemption_owner_rq_dash_only_fails(self) -> None:
        payload = self._growth_payload(
            self._stock_keys(["mix|Foo|zh"]),
            exemptions={"mix|Foo|zh": {"owner": "RQ-", "reason": "truncated id"}},
        )
        with self.assertRaises(localization_purity.PurityError) as ctx:
            localization_purity.check_baseline_growth(payload)
        self.assertIn("mix|Foo|zh", str(ctx.exception))

    def test_exemption_owner_bug_foo_fails(self) -> None:
        payload = self._growth_payload(
            self._stock_keys(["mix|Foo|zh"]),
            exemptions={"mix|Foo|zh": {"owner": "BUG-foo", "reason": "not a ticket id"}},
        )
        with self.assertRaises(localization_purity.PurityError) as ctx:
            localization_purity.check_baseline_growth(payload)
        self.assertIn("mix|Foo|zh", str(ctx.exception))

    def test_exempted_baseline_growth_with_ticket_owner_passes(self) -> None:
        payload = self._growth_payload(
            self._stock_keys(["mix|Foo|zh"]),
            exemptions={
                "mix|Foo|zh": {
                    "owner": "RQ-20260999-01",
                    "reason": "authorized one-off growth",
                }
            },
        )
        localization_purity.check_baseline_growth(payload)

    def test_dead_exemption_fails(self) -> None:
        payload = self._growth_payload(
            self._stock_keys(),
            exemptions={
                "mix|Ghost|zh": {"owner": "BUG-20260999-01", "reason": "no such key"}
            },
        )
        with self.assertRaises(localization_purity.PurityError) as ctx:
            localization_purity.check_baseline_growth(payload)
        self.assertIn("mix|Ghost|zh", str(ctx.exception))

    def test_exemption_owner_must_match_ticket_prefix(self) -> None:
        payload = self._growth_payload(
            self._stock_keys(["mix|Foo|zh"]),
            exemptions={
                "mix|Foo|zh": {"owner": "TICKET-1", "reason": "not a ticket id"}
            },
        )
        with self.assertRaises(localization_purity.PurityError) as ctx:
            localization_purity.check_baseline_growth(payload)
        self.assertIn("mix|Foo|zh", str(ctx.exception))

    def test_exemption_reason_must_be_nonempty(self) -> None:
        payload = self._growth_payload(
            self._stock_keys(["mix|Foo|zh"]),
            exemptions={"mix|Foo|zh": {"owner": "GOV-20260999-01", "reason": "  "}},
        )
        with self.assertRaises(localization_purity.PurityError) as ctx:
            localization_purity.check_baseline_growth(payload)
        self.assertIn("mix|Foo|zh", str(ctx.exception))

    def test_shrinking_baseline_without_lowering_cap_passes(self) -> None:
        localization_purity.check_baseline_growth(
            self._growth_payload(self._stock_keys()[:20])
        )

    def test_committed_baseline_pins_schema_cap_and_empty_exemptions(self) -> None:
        payload = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
        self.assertEqual(payload["schema"], 2)
        self.assertEqual(payload["cap"], 82)
        self.assertEqual(payload.get("stockKeys"), list(self.COMMITTED_STOCK_KEYS))
        self.assertEqual(len(payload.get("stockKeys", [])), 82)
        self.assertEqual(
            set(payload.get("growthExemptions", {})),
            {
                "ui|update/HarkenSearchUpdateNoticeUi.cpp|TableSetupColumn|73214a24e9dc6de4",
                "ui|update/HarkenSearchUpdateNoticeUi.cpp|TableSetupColumn|96e13b098a64396a",
            },
        )
        localization_purity.check_baseline_growth(payload)
        stored = localization_purity.load_baseline(BASELINE_PATH)
        self.assertIsInstance(stored, set)
        # The nine QA-narrator strings remain frozen in stockKeys, but are no
        # longer present in the main-only product source or active baseline.
        self.assertEqual(len(stored), 73)


if __name__ == "__main__":
    unittest.main()
