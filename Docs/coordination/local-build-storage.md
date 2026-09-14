# 本机共享依赖与磁盘存储

2026-09-12 用户授权将构建依赖抽到 D 盘全局目录、迁移 C 盘 worktrees 并清理可再生成临时产物。本记录仅描述这台 Windows 机器，不是其他开发者的固定磁盘要求。

## 新建构建环境

本机公共依赖前缀为 `D:/Tools/3DDY/deps/orca-msvc2022-x64-20260912`，环境入口为 `D:/Tools/3DDY/env.ps1`。在 PowerShell 中点调用该入口，再给 CMake 显式传入 `-DCMAKE_PREFIX_PATH=$env:ORCA_DEPS_PREFIX`。源码目录、构建目录、运行目录仍须按项目和验证任务隔离。

公共前缀是已经安装的第三方依赖和 Python 运行时，按版本只读复用。升级应创建新的并列目录。不要把任一项目的 CMakeCache、编译对象或 PDB 当公共依赖共享。MSVC、Windows SDK 和 CMake 保持在 Visual Studio 安装器管理的原位置。

## 旧路径兼容

旧依赖前缀 `D:/Workspace/06_3DDY_claude/deps/build/OrcaSlicer_dep/usr/local` 已改为指向上述公共前缀的 NTFS junction。现有开发与验证缓存可以继续使用旧逻辑路径；新构建使用公共前缀。不要删除公共依赖目标或将兼容链接当成冗余副本递归清理。

第二轮 worktree 迁移已完成，`D:/Tools/3DDY/migration-20260912-r2/result.json` 记录 `Status=COMPLETE`。实际存储为 `D:/Codex/worktrees`，原 `C:/Users/ltj/.codex/worktrees` 已成为指向 D 盘的兼容 junction；从原入口写入的文件实际落在 D 盘。414,797 个文件全部通过 SHA-256 比较，Git、未提交修改、验证配置路径、资源链接及写入检查通过，C 盘已核验原副本已清除。现有任务、Git worktree 登记、构建缓存、验证 registry 及冻结报告继续保留原 C 盘逻辑路径。迁移记录见 `D:/Tools/3DDY/migration-20260912-r2/RESULT.md`，第一轮维护历史见 `D:/Tools/3DDY/maintenance-20260912/RESULT.md`。不要把 C 盘兼容入口当冗余副本清理。

## 本轮清理范围

仅删除经清单确认的旧构建编译中间文件，以及固定验证任务确认的 `_CPack_Packages` 中间目录。保留当前开发与固定验证增量构建、源码、未提交修改、历史模型、真实任务配置、验证证据、交接登记、最新交付成品及各轮顶层包和清单。`.obj` 只有位于编译器对象目录时才按编译文件处理，三维模型不在清理范围。

本机详细文件清单、哈希和清理结果保存在 `D:/Tools/3DDY/maintenance-20260912/`，不进入 Git 源码历史。
