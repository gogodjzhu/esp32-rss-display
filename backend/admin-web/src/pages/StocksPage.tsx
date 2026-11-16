import { useCallback, useEffect, useRef, useState } from 'react';
import {
  Alert,
  App,
  Button,
  Card,
  Empty,
  Grid,
  Input,
  InputNumber,
  Skeleton,
  Space,
  Switch,
  Table,
  Tag,
  Tooltip,
  Typography,
  theme as antdTheme,
} from 'antd';
import type { ColumnsType } from 'antd/es/table';
import {
  ArrowDownOutlined,
  ArrowUpOutlined,
  DeleteOutlined,
  PlusOutlined,
  ReloadOutlined,
  SaveOutlined,
} from '@ant-design/icons';
import { api } from '../api';
import { fromNow, formatTime } from '../format';
import { useAutoRefresh } from '../hooks';
import type { StockConfig, StockRuntime, StockSymbol } from '../types';

const { Text } = Typography;

// 编辑中的行需要稳定 key，编号本身会被用户随时改，不能当 key。
interface EditableSymbol extends StockSymbol {
  key: number;
}

interface Props {
  active: boolean;
}

// 空行（编号和名称都没填）视为用户误加，保存前直接忽略。
function toPayload(symbols: EditableSymbol[]): StockSymbol[] {
  return symbols
    .map(({ code, name }) => ({ code: code.trim(), name: name.trim() }))
    .filter((symbol) => symbol.code !== '' || symbol.name !== '');
}

function marketOf(code: string): { label: string; color: string } | null {
  switch (code.trim().slice(0, 2).toLowerCase()) {
    case 'sh':
    case 'sz':
    case 'bj':
      return { label: 'A 股', color: 'blue' };
    case 'hk':
      return { label: '港股', color: 'purple' };
    case 'us':
      return { label: '美股', color: 'green' };
    default:
      return null;
  }
}

function RuntimeStatus({ enabled, runtime }: { enabled: boolean; runtime: StockRuntime }) {
  if (!enabled) {
    return <Text type="secondary">行情栏已停用，设备响应不附带 stocks 字段；打开开关并保存后恢复。</Text>;
  }
  const missing = runtime.missing_codes ?? [];
  return (
    <Space direction="vertical" size={8} style={{ width: '100%' }}>
      {runtime.last_error ? (
        <Alert
          type="error"
          showIcon
          message="最近一次拉取行情失败"
          description={`${runtime.last_error}（${fromNow(runtime.last_attempt_at)}）`}
        />
      ) : runtime.last_success_at ? (
        <Text type="secondary">
          <Tooltip title={formatTime(runtime.last_success_at)}>
            <span>最近成功 {fromNow(runtime.last_success_at)}</span>
          </Tooltip>
          {` · 当前缓存 ${runtime.quote_count} 条行情`}
        </Text>
      ) : (
        <Text type="secondary">还没有拉到行情；启用后后台会立即按当前编号拉取一次。</Text>
      )}
      {missing.length > 0 && (
        <Alert
          type="warning"
          showIcon
          message={`数据源没有返回这些编号：${missing.join('、')}`}
        />
      )}
    </Space>
  );
}

/** 行情配置页：启用开关、刷新间隔与编号列表，保存后立即作用于运行中的服务。 */
export function StocksPage({ active }: Props) {
  const { message, modal } = App.useApp();
  const { token } = antdTheme.useToken();
  const screens = Grid.useBreakpoint();
  const compact = !screens.md;
  const [config, setConfig] = useState<StockConfig | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [saving, setSaving] = useState(false);

  // 表单状态：保存时整份提交，顺序即设备端的轮播顺序
  const [enabled, setEnabled] = useState(false);
  const [refreshSeconds, setRefreshSeconds] = useState(30);
  const [symbols, setSymbols] = useState<EditableSymbol[]>([]);
  const keySeq = useRef(0);

  const applyConfig = useCallback((next: StockConfig) => {
    setConfig(next);
    setEnabled(next.enabled);
    setRefreshSeconds(next.refresh_seconds);
    setSymbols(next.symbols.map((symbol, index) => ({ ...symbol, key: index })));
    keySeq.current = next.symbols.length;
  }, []);

  const load = useCallback(async () => {
    setLoading(true);
    try {
      applyConfig(await api.stocks());
      setError('');
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setLoading(false);
    }
  }, [applyConfig]);

  useEffect(() => {
    void load();
  }, [load]);

  // 轮询只合并运行状态，不覆盖用户正在编辑的配置
  const pollRuntime = useCallback(async () => {
    try {
      const next = await api.stocks();
      setConfig((prev) => (prev ? { ...prev, runtime: next.runtime } : prev));
    } catch {
      // 状态轮询失败不打扰用户，保存/刷新时会有明确错误
    }
  }, []);
  useAutoRefresh(active, 15_000, () => void pollRuntime());

  const payload = toPayload(symbols);
  const dirty =
    config !== null &&
    (enabled !== config.enabled ||
      refreshSeconds !== config.refresh_seconds ||
      JSON.stringify(payload) !== JSON.stringify(config.symbols));

  function addSymbol() {
    setSymbols((prev) => [...prev, { key: keySeq.current++, code: '', name: '' }]);
  }

  function updateSymbol(key: number, patch: Partial<StockSymbol>) {
    setSymbols((prev) => prev.map((row) => (row.key === key ? { ...row, ...patch } : row)));
  }

  function removeSymbol(key: number) {
    setSymbols((prev) => prev.filter((row) => row.key !== key));
  }

  function moveSymbol(index: number, offset: number) {
    setSymbols((prev) => {
      const target = index + offset;
      if (target < 0 || target >= prev.length) return prev;
      const next = [...prev];
      [next[index], next[target]] = [next[target], next[index]];
      return next;
    });
  }

  function refreshStatus() {
    if (!dirty) {
      void load();
      return;
    }
    modal.confirm({
      title: '放弃未保存的修改？',
      content: '刷新状态会重新读取已保存的配置，当前未保存的修改会丢失。',
      okText: '刷新',
      cancelText: '取消',
      onOk: load,
    });
  }

  async function save() {
    if (payload.some((symbol) => symbol.code === '')) {
      message.error('有编号为空的行，请填写编号或删除该行');
      return;
    }
    if (enabled && payload.length === 0) {
      message.error('启用行情前至少要添加一个编号');
      return;
    }

    setSaving(true);
    try {
      const saved = await api.updateStocks({
        enabled,
        refresh_seconds: refreshSeconds,
        symbols: payload,
      });
      applyConfig(saved);
      message.success('已保存，后台正在按新配置拉取行情');
    } catch (err) {
      message.error(err instanceof Error ? err.message : String(err));
    } finally {
      setSaving(false);
    }
  }

  if (!config && loading) {
    return (
      <Card>
        <Skeleton active paragraph={{ rows: 6 }} />
      </Card>
    );
  }

  const columns: ColumnsType<EditableSymbol> = [
    {
      title: '顺序',
      key: 'order',
      width: 64,
      align: 'center',
      render: (_, __, index) => <Text type="secondary">{index + 1}</Text>,
    },
    {
      title: '行情编号',
      key: 'code',
      width: 220,
      render: (_, row) => (
        <Input
          size="small"
          value={row.code}
          placeholder="sh000001 / usAAPL"
          allowClear
          onChange={(event) => updateSymbol(row.key, { code: event.target.value })}
        />
      ),
    },
    {
      title: '显示名称',
      key: 'name',
      render: (_, row) => (
        <Input
          size="small"
          value={row.name}
          placeholder="留空则使用数据源名称"
          allowClear
          onChange={(event) => updateSymbol(row.key, { name: event.target.value })}
        />
      ),
    },
    {
      title: '市场',
      key: 'market',
      width: 84,
      render: (_, row) => {
        const market = marketOf(row.code);
        return market ? <Tag color={market.color}>{market.label}</Tag> : <Text type="secondary">—</Text>;
      },
    },
    {
      title: '轮播顺序',
      key: 'actions',
      width: 132,
      render: (_, row, index) => (
        <Space size={0}>
          <Tooltip title="上移">
            <Button
              type="text"
              size="small"
              icon={<ArrowUpOutlined />}
              disabled={index === 0}
              aria-label="上移"
              onClick={() => moveSymbol(index, -1)}
            />
          </Tooltip>
          <Tooltip title="下移">
            <Button
              type="text"
              size="small"
              icon={<ArrowDownOutlined />}
              disabled={index === symbols.length - 1}
              aria-label="下移"
              onClick={() => moveSymbol(index, 1)}
            />
          </Tooltip>
          <Tooltip title="删除">
            <Button
              type="text"
              size="small"
              danger
              icon={<DeleteOutlined />}
              aria-label="删除"
              onClick={() => removeSymbol(row.key)}
            />
          </Tooltip>
        </Space>
      ),
    },
  ];

  return (
    <Space direction="vertical" size={16} style={{ width: '100%', paddingBottom: 96 }}>
      {error && <Alert type="error" showIcon message="加载行情配置失败" description={error} />}

      {!config ? (
        <Empty description="暂无行情配置" />
      ) : (
        <>
          <Card
            size="small"
            title="行情栏"
            extra={
              <Button size="small" icon={<ReloadOutlined />} loading={loading} onClick={refreshStatus}>
                刷新状态
              </Button>
            }
          >
            <Space direction="vertical" size={12} style={{ width: '100%' }}>
              <Space size={24} wrap>
                <Space size={8}>
                  <Text>启用行情栏</Text>
                  <Switch
                    checked={enabled}
                    onChange={setEnabled}
                    checkedChildren="启用"
                    unCheckedChildren="停用"
                  />
                </Space>
                <Space size={8}>
                  <Text>刷新间隔</Text>
                  <InputNumber
                    min={5}
                    max={3600}
                    addonAfter="秒"
                    style={{ width: 130 }}
                    value={refreshSeconds}
                    onChange={(value) => setRefreshSeconds(value ?? 30)}
                  />
                </Space>
              </Space>
              <RuntimeStatus enabled={enabled} runtime={config.runtime} />
            </Space>
          </Card>

          <Card
            size="small"
            title="股票 / 指数编号"
            extra={
              <Button size="small" icon={<PlusOutlined />} onClick={addSymbol}>
                新增编号
              </Button>
            }
          >
            <Space direction="vertical" size={12} style={{ width: '100%' }}>
              <Table<EditableSymbol>
                rowKey="key"
                size="small"
                columns={columns}
                dataSource={symbols}
                pagination={false}
                scroll={{ x: 'max-content' }}
                locale={{ emptyText: '还没有行情编号，点「新增编号」添加' }}
              />
            </Space>
          </Card>

          {/* 编号列表可能很长，保存按钮固定在视口底部，改完任意一行都能直接保存 */}
          <div
            style={{
              position: 'fixed',
              left: 0,
              right: 0,
              bottom: 0,
              zIndex: 20,
              background: token.colorBgContainer,
              borderTop: `1px solid ${token.colorBorderSecondary}`,
              boxShadow: '0 -2px 8px rgba(0, 0, 0, 0.06)',
            }}
          >
            <div
              style={{
                maxWidth: 1400,
                margin: '0 auto',
                padding: `10px ${compact ? 12 : 24}px`,
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'space-between',
                gap: 12,
                flexWrap: 'wrap',
              }}
            >
              <Space wrap>
                <Button
                  type="primary"
                  icon={<SaveOutlined />}
                  loading={saving}
                  disabled={!dirty}
                  onClick={() => void save()}
                >
                  保存配置
                </Button>
                <Button disabled={!dirty || loading} onClick={refreshStatus}>
                  放弃修改
                </Button>
              </Space>
              {!compact && (
                <Text type="secondary" style={{ fontSize: 12 }}>
                  保存后立即生效，列表顺序即设备端轮播顺序。
                </Text>
              )}
            </div>
          </div>

          <Text type="secondary" style={{ fontSize: 12 }}>
            编号使用腾讯行情前缀：sh/sz/bj 为 A 股与指数，hk 为港股，us 为美股（代码自动转大写）。
            显示名称留空则使用数据源名称。config.yaml 的 stocks 只作为首次启动的种子，之后以本页配置为准。
          </Text>
        </>
      )}
    </Space>
  );
}
