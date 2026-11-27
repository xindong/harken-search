# Windows 安装器

直接双击安装包仍走普通安装：分别确认许可协议和隐私政策，不必滚动到底，也不会为以后的静默安装保存同意。

## 先记录同意，再静默安装

只适用于当前登录 Windows 桌面的管理员本人。两步使用同一台电脑、同一账号、同一安装包和同一语种；系统服务账号和代签不适用。

在已经提升的命令行中执行：

1. `HarkenSearch_Setup_v1.0.47.exe /ALLUSERS /SP- /LANG=chinesesimp /RECORDCONSENT /LOG=consent.log`。本人阅读并分别同意两份文件，点「保存同意」。以「同意记录已保存，未安装软件」及日志里的 `Installer consent saved` 为准。这个模式在安装前关闭向导，退出码 `2` 本身不表示已保存。
2. 30 分钟内执行 `HarkenSearch_Setup_v1.0.47.exe /ALLUSERS /SP- /LANG=chinesesimp /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=install.log`。退出码 `0` 表示安装成功。每份记录只授权一次安装；开始更改系统之前即消耗，开始安装后失败也需重新确认。

没有匹配记录时，安装器记录原因并退出，不安装或替换旧版本。安装包、协议文本、语种或账号改变时重新确认。需迁移旧 Aris 安装时改用普通交互安装。

待用记录只存在本机受 Windows 管理员权限保护的注册表，以账号标识区分，记录安装包和两份文本的摘要、语言、版本及确认时间。不接受「同意」开关或外部声明文件，不支持企业代签、跨机凭据或绕过 Windows 提权。
