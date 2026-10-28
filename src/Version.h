#pragma once

// SoundManager 版本管理

// 版本定义：
//   SOUNDMANAGER_EDITION_ENTRY    - 入门版 (基础功能)
//   SOUNDMANAGER_EDITION_STANDARD - 标准版 (完整功能)
//   SOUNDMANAGER_EDITION_ULTIMATE - Ultimate (all feature gates enabled)

// 版本号
#define SOUNDMANAGER_VERSION_MAJOR 1
#define SOUNDMANAGER_VERSION_MINOR 0
#define SOUNDMANAGER_VERSION_PATCH 41
#ifndef ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION
    #define SOUNDMANAGER_VERSION_STRING "1.0.41"
#else
    #define SOUNDMANAGER_VERSION_STRING ARIS_UPDATE_LOCAL_DEMO_BUILD_VERSION
#endif

// 版本类型 (编译时定义，默认为标准版)
#ifndef SOUNDMANAGER_EDITION
    #define SOUNDMANAGER_EDITION 2  // 默认标准版
#endif

// 版本常量
#define SOUNDMANAGER_EDITION_ENTRY    1
#define SOUNDMANAGER_EDITION_STANDARD 2
#define SOUNDMANAGER_EDITION_ULTIMATE 3

// 版本名称
#if SOUNDMANAGER_EDITION == SOUNDMANAGER_EDITION_ENTRY
    #define SOUNDMANAGER_EDITION_NAME "Entry"
    #define SOUNDMANAGER_EDITION_NAME_CN u8"入门版"
#elif SOUNDMANAGER_EDITION == SOUNDMANAGER_EDITION_ULTIMATE
    #define SOUNDMANAGER_EDITION_NAME "Ultimate"
    #define SOUNDMANAGER_EDITION_NAME_CN u8"终极版"
#else
    #define SOUNDMANAGER_EDITION_NAME "Standard"
    #define SOUNDMANAGER_EDITION_NAME_CN u8"标准版"
#endif

// 功能开关宏

// 入门版功能 (所有版本都有)
#define FEATURE_BASIC_PLAYBACK          1   // 基础播放
#define FEATURE_WAVEFORM_DISPLAY        1   // 波形显示
#define FEATURE_FILE_BROWSER            1   // 文件浏览器
#define FEATURE_BASIC_SEARCH            1   // 基础搜索
#define FEATURE_DATABASE                1   // 数据库管理

// 标准版及以上功能
#if SOUNDMANAGER_EDITION >= SOUNDMANAGER_EDITION_STANDARD
    #define FEATURE_MULTI_DATABASE      1   // 多数据库支持
    #define FEATURE_BATCH_RENAME        1   // 批量重命名
    #define FEATURE_PLAYLISTS           1   // 播放列表
    #define FEATURE_METADATA_EDIT       1   // 元数据编辑
    #define FEATURE_SPECTROGRAM         1   // 频谱图
    #define FEATURE_DRAG_DROP_DAW       1   // 拖放到DAW
    #define FEATURE_PITCH_SHIFT         1   // 变调播放
    #define FEATURE_REGION_EXPORT       1   // 区域导出
    #define FEATURE_ALBUM_ART           1   // 专辑封面
#else
    #define FEATURE_MULTI_DATABASE      0
    #define FEATURE_BATCH_RENAME        0
    #define FEATURE_PLAYLISTS           0
    #define FEATURE_METADATA_EDIT       0
    #define FEATURE_SPECTROGRAM         0
    #define FEATURE_DRAG_DROP_DAW       0
    #define FEATURE_PITCH_SHIFT         0
    #define FEATURE_REGION_EXPORT       0
    #define FEATURE_ALBUM_ART           0
#endif

// Ultimate-only feature gates.
#if SOUNDMANAGER_EDITION >= SOUNDMANAGER_EDITION_ULTIMATE
    #define FEATURE_REVERSE_PLAYBACK    1   // 反向播放
    #define FEATURE_COVER_GALLERY       1   // 封面画廊
    #define FEATURE_ADVANCED_FILTERS    1   // 高级筛选
    #define FEATURE_BATCH_METADATA      1   // 批量元数据编辑
    #define FEATURE_FOLDER_AUTO_RENAME  1   // 文件夹自动重命名
    #define FEATURE_MULTI_TAB           1   // 多标签页
    #define FEATURE_KEYBOARD_SHORTCUTS  1   // 自定义快捷键
#else
    #define FEATURE_REVERSE_PLAYBACK    0
    #define FEATURE_COVER_GALLERY       0
    #define FEATURE_ADVANCED_FILTERS    0
    #define FEATURE_BATCH_METADATA      0
    #define FEATURE_FOLDER_AUTO_RENAME  0
    #define FEATURE_MULTI_TAB           0
    #define FEATURE_KEYBOARD_SHORTCUTS  0
#endif

// 便捷检查宏
#define IS_ENTRY_EDITION    (SOUNDMANAGER_EDITION == SOUNDMANAGER_EDITION_ENTRY)
#define IS_STANDARD_EDITION (SOUNDMANAGER_EDITION == SOUNDMANAGER_EDITION_STANDARD)
#define IS_ULTIMATE_EDITION (SOUNDMANAGER_EDITION == SOUNDMANAGER_EDITION_ULTIMATE)
#define IS_STANDARD_OR_ABOVE (SOUNDMANAGER_EDITION >= SOUNDMANAGER_EDITION_STANDARD)
#define IS_ULTIMATE_ONLY    (SOUNDMANAGER_EDITION >= SOUNDMANAGER_EDITION_ULTIMATE)
