# CAMP Fluent Bit 5.1.5

从仓库根目录执行，在目标架构原生构建：

```sh
docker build --network host --build-arg BUILD_JOBS=8 \
  -f dockerfiles/Dockerfile.camp -t <registry>/camp/fluent-bit:5.1.5-lgpage16-arm64 .
```

ARM64 的 jemalloc 使用 `--with-lg-quantum=3 --with-lg-page=16`；5.1.5 上游 CMake 已对 Linux AArch64 启用 signed char。
最终镜像仅复制一份运行库、配置和新二进制，不含构建工具或旧二进制层。运行库来自固定 digest 的官方 5.1.5 镜像。
构建上下文必须保留 vendored `lib/*/build` 目录，只排除仓库根目录的 build 和 .git；不要使用无路径前缀的 tar `--exclude=build`。
发布前须在原生 4KB/64KB ARM64 和 amd64 核对程序版本、Unicode、状态恢复和插件配置；amd64 可复用已验收的官方子镜像。
