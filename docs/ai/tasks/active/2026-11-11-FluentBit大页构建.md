# Fluent Bit 5.1.5 ARM64 大页构建

基线：上游 v5.1.5，0039c4f0d1b530bedd8448df2fa4c301d58a5b91。
分支：build/v5.1.5-arm64-64k。

- 使用 dockerfiles/Dockerfile.camp 原生构建 ARM64，jemalloc lg-page=16（64KB）。
- 只改变构建和打包；保留上游生产插件选项，最终 COPY 一个运行层。
- amd64 复用官方 v5.1.5 子 manifest，最终交付多架构索引。
- 验收：67/183/15 隔离状态与 Unicode 输出；不升级原集群采集器。
- 实际 digest、体积和结果以 ai-workspace 的本次构建发布记录为准。
