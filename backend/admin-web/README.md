# admin-web

管理页前端源码：Vite + React 18 + TypeScript + Ant Design 5 + Recharts。

构建产物输出到 `../server/admin/static/`，由 `server/admin/admin.go` 的 `go:embed`
打进二进制。**产物已入库**，因此 Go 构建与 Docker 构建都不需要 Node。

## 开发

```sh
npm install
npm run dev      # http://localhost:5173/admin/ ，接口代理到 localhost:8080
```

先另开一个终端跑后端：`cd .. && go run ./cmd/server --config config.yaml`。

## 构建

```sh
npm run build    # 输出到 ../server/admin/static
```

改完前端务必重新构建并提交产物，否则线上页面不会变化。

## 结构

- `src/App.tsx`：布局、主题（亮色/暗色/跟随系统）、自动刷新、两个 tab
- `src/pages/OverviewPage.tsx`：「概览」统计卡片与图表
- `src/pages/FeedsPage.tsx`：「订阅源」表格与操作菜单
- `src/components/FeedFormModal.tsx`：新增/编辑弹窗，新增前必须校验预览
- `src/components/FeedItemsDrawer.tsx`：单源条目抽屉
- `src/api.ts`、`src/types.ts`：后端接口封装与类型
