import { useState } from 'react';
import {
  Alert,
  App as AntApp,
  Button,
  ConfigProvider,
  Grid,
  Layout,
  Segmented,
  Space,
  Switch,
  Tabs,
  Tooltip,
  Typography,
  theme as antdTheme,
} from 'antd';
import {
  DatabaseOutlined,
  DesktopOutlined,
  MoonOutlined,
  ReloadOutlined,
  SunOutlined,
} from '@ant-design/icons';
import zhCN from 'antd/locale/zh_CN';
import { useAdminStatus, useAutoRefresh } from './hooks';
import { useThemeMode } from './theme';
import type { ThemeMode } from './theme';
import { OverviewPage } from './pages/OverviewPage';
import { FeedsPage } from './pages/FeedsPage';

const { Text } = Typography;

const REFRESH_MS = 30_000;

interface ShellProps {
  mode: ThemeMode;
  onModeChange: (mode: ThemeMode) => void;
}

function Shell({ mode, onModeChange }: ShellProps) {
  const { token } = antdTheme.useToken();
  const screens = Grid.useBreakpoint();
  const { status, loading, error, reload } = useAdminStatus();
  const [tab, setTab] = useState('overview');
  const [autoRefresh, setAutoRefresh] = useState(true);
  const [refreshing, setRefreshing] = useState(false);

  useAutoRefresh(autoRefresh, REFRESH_MS, () => void reload(true));

  async function refresh() {
    setRefreshing(true);
    await reload(true);
    setRefreshing(false);
  }

  const compact = !screens.md;

  return (
    <Layout style={{ minHeight: '100vh' }}>
      <Layout.Header
        style={{
          position: 'sticky',
          top: 0,
          zIndex: 20,
          display: 'flex',
          alignItems: 'center',
          gap: 12,
          paddingInline: compact ? 12 : 24,
          background: token.colorBgContainer,
          borderBottom: `1px solid ${token.colorBorderSecondary}`,
          height: 56,
          lineHeight: 'normal',
        }}
      >
        <Space size={8} style={{ flex: 1, minWidth: 0 }}>
          <DatabaseOutlined style={{ fontSize: 20, color: token.colorPrimary }} />
          <Text strong style={{ fontSize: 16, whiteSpace: 'nowrap' }}>
            esp32-rss
          </Text>
          {!compact && <Text type="secondary">管理台</Text>}
        </Space>

        <Space size={compact ? 6 : 12} wrap={false}>
          {!compact && (
            <Space size={6}>
              <Text type="secondary" style={{ fontSize: 12 }}>
                自动刷新
              </Text>
              <Switch size="small" checked={autoRefresh} onChange={setAutoRefresh} />
            </Space>
          )}
          <Segmented<ThemeMode>
            size={compact ? 'small' : 'middle'}
            value={mode}
            onChange={onModeChange}
            options={[
              { value: 'auto', icon: <DesktopOutlined />, title: '跟随系统' },
              { value: 'light', icon: <SunOutlined />, title: '浅色' },
              { value: 'dark', icon: <MoonOutlined />, title: '深色' },
            ]}
          />
          <Tooltip title="重新拉取数据">
            <Button
              icon={<ReloadOutlined />}
              loading={refreshing}
              onClick={refresh}
              aria-label="刷新"
            />
          </Tooltip>
        </Space>
      </Layout.Header>

      <Layout.Content style={{ padding: compact ? 12 : 24, maxWidth: 1400, width: '100%', margin: '0 auto' }}>
        {error && (
          <Alert
            type="error"
            showIcon
            closable
            message="加载运行状态失败"
            description={error}
            style={{ marginBottom: 12 }}
          />
        )}
        <Tabs
          activeKey={tab}
          onChange={setTab}
          items={[
            {
              key: 'overview',
              label: '概览',
              children: <OverviewPage status={status} loading={loading} />,
            },
            {
              key: 'feeds',
              label: `订阅源${status ? `（${status.feeds.length}）` : ''}`,
              children: (
                <FeedsPage
                  feeds={status?.feeds ?? []}
                  loading={loading}
                  onChanged={() => void reload(true)}
                />
              ),
            },
          ]}
        />
      </Layout.Content>
    </Layout>
  );
}

export default function App() {
  // 主题状态只在这里持有，避免多处 useThemeMode 各自读写 localStorage 而不同步。
  const { mode, setMode, dark } = useThemeMode();
  const algorithm = dark ? antdTheme.darkAlgorithm : antdTheme.defaultAlgorithm;

  return (
    <ConfigProvider
      locale={zhCN}
      theme={{
        algorithm,
        token: {
          colorPrimary: '#1677ff',
          borderRadius: 8,
          fontSize: 14,
        },
      }}
    >
      <AntApp>
        <Shell mode={mode} onModeChange={setMode} />
      </AntApp>
    </ConfigProvider>
  );
}
