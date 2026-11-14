# 共享记账 - Lab3提交源码

本目录保留应用源码、测试源码、界面资源、CMake配置和必要的构建工具。

## 编译和运行

环境：Windows x64、Visual Studio 2022“使用C++的桌面开发”（含Windows SDK、CMake、Ninja），Qt 6.8.3 MSVC x64。

在本目录打开PowerShell：
1. 首次准备Qt：
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Setup.ps1
   已有兼容Qt SDK时，可将当前会话QT_ROOT指向SDK根目录，跳过下载。
2. 编译并执行后台测试：.\build.cmd
3. 启动桌面应用：.\run.cmd

没有默认账号。从“注册”创建自己的账号；创建账本者为管理员，可按用户名邀请其他已注册用户，对方接受后加入。记账时选择实际付款人和费用承担人，再提交、审核和结算。

版本0.9.4新增账号安全。新注册后请保存仅当次展示的恢复码。旧账号登录后在右上角“账号安全”验证当前密码生成恢复码，也可修改密码；登录页“忘记密码”通过此前保存的恢复码重置。修改或重置后全部旧会话失效，账本和账目保留。旧恢复码重新生成/使用后失效；未设恢复码且忘记密码，或密码和恢复码同时遗失时，不能自助恢复。账本管理员不能代改其他账号密码。

当前需求及本轮变更说明位于docs/requirements.md和docs/account-security-change.md。

## 文件用途

- src/：手写C++应用代码。
- tests/：自动测试源码，CMake默认构建和测试需要，故保留。
- resources/：界面样式，运行时由Qt资源系统加载。
- scripts/：环境、编译、运行、风格、代码量和桌面验证工具。
- licenses/：第三方组件许可文本。
- CMakeLists.txt / CMakePresets.json / .clang-format：构建和风格配置。

检查代码量：scripts/Measure-Code.ps1。
检查格式：scripts/Format.ps1。
原生桌面验证：scripts/Verify-Desktop.ps1 -IdentityFlows、-SecurityFlows或-RecoveryFlows，每次选一套；测试使用独立临时数据。

UML图及PlantUML源文件位于外层UML目录。实验报告PDF位于与code、UML并列的位置。
本源码包不包含阶段验收文档、变更记录、UI原型、运行数据库、SDK或旧构建输出。