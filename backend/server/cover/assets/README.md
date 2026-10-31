# 封面字体

`cjk_subset.ttf` 复制自仓库根目录的 `assets/fonts/cjk_subset.ttf`，用于后端按
订阅源名称生成兜底封面。字体来源、裁剪范围与许可见
[`assets/fonts/README.md`](../../../../assets/fonts/README.md)。

后端需要该字体随二进制发布，因此通过 `go:embed` 打包；修改字体后需要同步
更新这里的副本。
