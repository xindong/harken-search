# 安装器语言文件

`ChineseSimplified.isl` 基于 Inno Setup 上游 `jrsoftware/issrc` 的 `Files/Languages/ChineseSimplified.isl`，适用于 Inno Setup 6.5.0+；与本机 6.7.1 编译器核验。

- 上游：https://github.com/jrsoftware/issrc/blob/main/Files/Languages/ChineseSimplified.isl
- 上游固定 Git blob：`d0d44258f51aa3127fc0d1167a7c8be328a9bdcf`
- 获取日期：2026-10-27。保留文件内原作者信息；上游许可证随仓库及产品保存在 `LICENSES/Inno-Setup.txt`。
- Harken Search 的专用文案在 `HarkenSearch_Setup.iss` 的 `[CustomMessages]` 中维护中英文，不改写上游翻译。
- 本地只启用 DialogFontName=Microsoft YaHei UI，避免干净 Windows 沙箱中的中文法律页与控件显示方块；上游固定 Git blob 仅标识获取基准，不对应本地修改后的文件。

安装器使用 Windows 用户显示语言（`GetUserDefaultUILanguage`），不使用地区/数字格式：简体中文选 `chinesesimp`，英文选 `english`，其他未支持语言回退英文。关闭语言选择窗口，禁用沿用上次安装语言，以便升级时也跟随当前系统显示语言。

官方规则：[语言检测](https://jrsoftware.org/ishelp/topic_setup_languagedetectionmethod.htm)、[旧语言记录](https://jrsoftware.org/ishelp/topic_setup_usepreviouslanguage.htm)、[语言匹配](https://jrsoftware.org/ishelp/topic_languagessection.htm)。
