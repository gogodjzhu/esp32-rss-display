import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// 构建产物直接落到 Go 的 embed 目录，部署时不需要 Node。
// base 必须是 /admin/，因为页面由 /admin/ 前缀提供。
export default defineConfig({
  plugins: [react()],
  base: '/admin/',
  build: {
    outDir: '../server/admin/static',
    emptyOutDir: true,
    // antd + recharts 单个 chunk 约 1.1 MB，gzip 后 350 KB；
    // 管理页按需加载，接受这个体积，避免误报警告。
    chunkSizeWarningLimit: 1200,
    rollupOptions: {
      output: {
        // 把体积大且很少变动的依赖拆开，浏览器缓存更耐久
        manualChunks: {
          antd: ['antd', '@ant-design/icons'],
          charts: ['recharts'],
        },
      },
    },
  },
  server: {
    // 本地开发时把接口请求转发给 go run ./cmd/server
    proxy: {
      '/v1': 'http://localhost:8080',
      '/admin/status': 'http://localhost:8080',
      '/admin/feeds': 'http://localhost:8080',
      '/image': 'http://localhost:8080',
      '/metrics': 'http://localhost:8080',
      '/nfc': 'http://localhost:8080',
    },
  },
});
