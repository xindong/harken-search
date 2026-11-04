# 本地相似音效模型：固定资源与复现

## 产品运行

产品只需要原生 ONNX Runtime 和三个资源文件，无 Python、联网推理或文本编码器。未配置模型时，听感接近仍可用；同类替代明确提示缺少模型。

- 模型：laion/clap-htsat-unfused，固定修订 8fa0f1c6d0433df6e97c127f64b2a1d6c0dcda8a。
- 上游模型卡：<https://huggingface.co/laion/clap-htsat-unfused/tree/8fa0f1c6d0433df6e97c127f64b2a1d6c0dcda8a>，该修订声明 Apache-2.0。
- ONNX Runtime CPU 1.20.1：<https://github.com/microsoft/onnxruntime/releases/tag/v1.20.1>。
- hnswlib 0.8.0，提交 3f3429661187e4c24a490a0f148fc6bc89042b3d；不配置时使用精确向量查找。

资源 SHA-256（程序加载时逐个验证）：

| 文件 | SHA-256 |
|---|---|
| clap-audio.onnx | 50d03fb781f7bf520a5d424a3479871cfca4af7a31026892ac1e935d5303f677 |
| clap-mel-filters.f64 | 256b8c586fb0f0ea44c6b2cafa44caa9bf288c1cfe1a05ff2527b73ae123d37a |
| clap-window.f64 | 0d9c0e8dfb7e477785eaaac6e2082e19a030bc113178a207cb4d1ab8d536a7a9 |

Windows SDK 压缩包 onnxruntime-win-x64-1.20.1.zip 的 SHA-256 为 78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581。运行时及模型许可证和第三方声明随 LICENSES/ 交付。

## 导出和核对

在独立 Python 3.12 环境安装 torch==2.5.1+cpu（PyTorch 官方 CPU 源）、transformers==4.46.3、onnx==1.17.0、onnxruntime==1.20.1、numpy==1.26.4、scipy==1.14.1、soundfile==0.12.1。首次运行会从固定上游修订下载构建输入；只允许在构建端执行，不由应用隐式下载。

运行：

    python tools/export_similarity_model.py --output <资源目录> --audio <本机真实音效.wav>

已有资源使用 --verify-existing，避免重新导出。工具采用固定 opset 17 / float32，输出与部署哈希不符直接失败。报告记录依赖版本、模型修订、哈希，以及官方 PyTorch 与 ONNX CPU 输出的差异。原生 TestAudioEmbeddingService 再比较真实音频和确定性合成音频的 mel 与向量。原始素材及验证用 PCM 不提交，不随包分发。

配置 CMake：HARKEN_ONNXRUNTIME_ROOT 指向 SDK，HARKEN_HNSWLIB_ROOT 指向固定头文件目录，HARKEN_SIMILARITY_MODEL_DIR 指向资源目录。HARKEN_SIMILARITY_MODEL_FIXTURES 另行启用本地模型测试和 SimilarityV2Bench；测试数据不是产品资源。

## 固定预处理

48 kHz 单声道；先声学分析确定首尾活动范围，保留中间静音。48 kHz 解码顺序流式读取，整段统一峰值归一化到 0.95。10 秒窗口、5 秒步长，尾段按官方 repeatpad 补齐，但记录与聚合权重只计算真实覆盖时长。统一全局增益，不逐窗抹平动态。

1024 点周期 Hann，480 点 hop，64 个 Slaney mel 滤波器，官方 dB/动态范围；固定输入 [1,1,1001,64]，输出 512 维单位向量。全文件聚合按每个窗口新增覆盖时长加权；深度比较另保留各窗口原始先后顺序。

缓存模型版本包含 repeatpad-slaney-peak095-stream-v2。升级该字符串会要求重新补充语义分析，不能把旧预处理的向量混到新版本里。22.05 kHz 声学边界换到 48 kHz 时，仅容忍最多 2 ms 的解码长度舍入，并以真实解码末尾为准。超出最大分析长度、损坏数据、模型缺失和取消各自返回失败，不静默截短或使用伪造向量。

## 验证边界

推理一致性不代表检索准确率。准确率须按 docs/plans/2026-11-01-similar-deep.md 的录音家族拆分和两种查找目的，完成独立试听标注后再计算 nDCG/P@10。未完成的人工标签、10 万条性能目标必须作为未验证项交付。
