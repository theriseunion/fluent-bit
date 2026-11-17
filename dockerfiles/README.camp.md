# CAMP Fluent Bit 5.1.5

从仓库根目录执行，在目标架构原生构建：

```sh
docker build --network host --build-arg BUILD_JOBS=8 \
  -f dockerfiles/Dockerfile.camp -t <registry>/camp/fluent-bit:5.1.5-lgpage16-arm64 .
```

ARM64 的 jemalloc 使用 `--with-lg-quantum=3 --with-lg-page=16`；5.1.5 上游 CMake 已对 Linux AArch64 启用 signed char。
最终镜像仅复制一份运行库、配置和新二进制，不含构建工具或旧二进制层。运行库来自固定 digest 的官方 5.1.5 镜像。
构建上下文必须保留 vendored `lib/*/build` 目录，只排除仓库根目录的 build 和 .git；不要使用无路径前缀的 tar `--exclude=build`。
发布前须在原生 4KB/64KB ARM64 和 amd64 核对程序版本、Unicode、状态恢复和插件配置。本分支修改了 S3 恢复代码，amd64/arm64 都必须从本分支重新构建，不能复用未修补的官方 amd64 子镜像。


本分支的 multipart 修复仍为候选，未发布替代现有镜像。旧 metadata 不保存 tag，
恢复 completion-only 对象进入 tag claim 检查时会访问空指针；修复对未知 tag 的完成请求
使用全局 claim，保留已有并发保护。新增 `multipart_restart_completion` 运行时回归。
本机最小调试构建的原有 27 项 S3 测试与新增回归通过；同一回归在原版退出 139。
真实 MinIO 的 3.0.4→候选、候选自身故障重启各恢复 3500 条，缺失/重复均为 0。
正式完整镜像、ARM64/64KiB、并发与故障重试验收仍待完成，不据此放行 CAMP 升级。
