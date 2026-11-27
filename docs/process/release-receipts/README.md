# 正式发布回执

每个正式版一个 `<version>.json`，记录这一版实际发布出去的确定事实：从哪个提交、哪次构建、哪个 run 发起，安装包与更新包的 SHA-256，HTML 清单与公告的哈希，签名主体与发布机器。用来回答「1.0.46 到底发的是什么」，不用再从线上清单反推。

这些文件只在这里维护，公开仓库投影通过 `tools/public_repo_manifest.py` 的 `docs/process/` 前缀整体排除，不进公开源码。

## 谁写

发布器 HarkenSearch Publishing Tool（`xindong/aris-publishing-tool`）第 6 步。GitHub 与 COS 两端都成功后：

1. 先写发布机的 `receipts/<version>.json`；
2. 配置了「回执归档目录」时，再写仓库回执 `docs/process/release-receipts/<version>.json`。

对已发布的版本补记不需要重新发布，在发布器仓库运行：

```powershell
.venv/Scripts/python.exe main.py --source-repository xindong/harken-search --branch main `
  archive-receipt --stage "<data_root>\staging\<run_id>" `
  --published "<data_root>\receipts\<version>.json" `
  --versions "<正式 versions.json>" --output-dir "<本仓库>\docs\process\release-receipts"
```

归档时读三份输入并互相核对：构建素材回执 `staging-manifest.json`（run、分支、来源提交、签名者、更新包哈希）、发布后回执 `receipts/<version>.json`（两端结果、全部文件哈希、清单与公告哈希）、正式 `versions.json`（这一版的安装包大小与 SHA-256）。任何一处对不上就不写。

## 字段

| 字段 | 含义 |
| --- | --- |
| `schema_version` | 当前为 1，字段变化时递增 |
| `version` / `channel` | 版本号与渠道（目前只有 stable） |
| `build_id` / `source_commit` | 构建标识与来源提交，两者都绑定同一 SHA |
| `run_id` | GitHub Actions 构建 run |
| `prepared_at` / `released_at` | 发布材料生成时间、正式发布时间 |
| `product` | 版本清单里的产品标识 |
| `github` | 发布仓库与 `v<version>` tag |
| `files` | 本次发布对象：文件名、字节数、SHA-256 |
| `manifests` | canonical / legacy 清单与签名的文件名和哈希 |
| `notes` | 官网公告对象 key 与哈希 |
| `signer` | Authenticode 主体与证书指纹 |
| `installer` / `update` | 安装包与更新 ZIP 的单条记录 |
| `published_from.machine` | 发布机器名，用于回查；不含本机用户名 |

`files` 里的文件名必须是单纯文件名；不写绝对路径。

## 写入规则

- 同名文件内容完全相同：视为已归档，不改动。
- 同名文件内容不同：拒绝覆盖。版本号不复用，补发或修正走下一个版本号。
- 不写本机用户名、COS 桶/地域/前缀等存储配置、任何凭据，也不写 staging 目录路径。
- 发布已经成功后归档失败，不回滚、不重发软件；日志提示手动补记。

## 怎么读

- 某个版本发了什么：看该版本 JSON 的 `installer.sha256` 与 `update.sha256`。
- 线上是不是同一份：把 `installer.sha256` 与 `versions.json` 同版本记录的 `setup.sha256` 对比。
- 来源是不是同一提交：`source_commit` 与发布 tag 的提交对比。
- 签名对不对：`signer.subject` 应为 `X.D. Network Inc.`，`signer.thumbprint` 与安装包实际证书对比。

`tests/test_release_receipt_contract.py` 会检查字段、文件名与版本号一致，并拦住用户名、存储配置、凭据和绝对路径这类内容。
