import { useMemo } from 'react';
import { Card, Col, Empty, Row, Skeleton, Space, Statistic, Tooltip, Typography, theme as antdTheme } from 'antd';
import {
  Bar,
  BarChart,
  CartesianGrid,
  Cell,
  ResponsiveContainer,
  Tooltip as ChartTooltip,
  XAxis,
  YAxis,
} from 'recharts';
import { fromNow, formatBytes, formatTime, isLater } from '../format';
import type { AdminStatus } from '../types';

const { Text } = Typography;

interface Props {
  status: AdminStatus | null;
  loading: boolean;
}

/** 概览页：只放能一眼看清运行情况的聚合数据与图表，明细留给订阅源页。 */
export function OverviewPage({ status, loading }: Props) {
  const { token } = antdTheme.useToken();

  const chartData = useMemo(
    () => (status?.items_by_day ?? []).map((d) => ({ ...d, label: d.date.slice(5) })),
    [status],
  );

  const ratingData = useMemo(() => {
    const dist = status?.ratings.dist ?? [0, 0, 0, 0, 0];
    return dist.map((count, i) => ({ label: `${i + 1}★`, count }));
  }, [status]);

  if (!status && loading) {
    return (
      <Card>
        <Skeleton active paragraph={{ rows: 6 }} />
      </Card>
    );
  }

  if (!status) {
    return <Empty description="暂无运行数据" />;
  }

  const failing = status.feeds.filter(
    (f) => f.enabled && f.last_error_at && isLater(f.last_error_at, f.last_success_at),
  );

  const todayCount = chartData.length ? chartData[chartData.length - 1].count : 0;

  const cards = [
    {
      title: '条目总数',
      value: status.items.total,
      sub: `近 24 小时新增 ${status.items.last_24h} 条`,
    },
    {
      title: '设备',
      value: status.devices.total,
      sub: `近 1 小时活跃 ${status.devices.active_1h} 台`,
    },
    {
      title: '今日新增',
      value: todayCount,
      sub: `保留 ${status.max_age_hours} 小时内的内容`,
    },
    {
      title: '平均评分',
      value: status.ratings.count ? status.ratings.avg.toFixed(1) : '—',
      sub: status.ratings.count ? `共 ${status.ratings.count} 次评分` : '还没有评分',
    },
    {
      title: '图片占用',
      value: formatBytes(status.images.bytes),
      sub: `${status.images.files} 个文件`,
    },
    {
      title: '数据库',
      value: formatBytes(status.database.bytes),
      sub: status.database.file || '—',
    },
  ];

  const chartTooltipStyle = {
    background: token.colorBgElevated,
    border: `1px solid ${token.colorBorderSecondary}`,
    borderRadius: token.borderRadius,
    color: token.colorText,
    fontSize: 12,
  };

  return (
    <Space direction="vertical" size={16} style={{ width: '100%' }}>
      <Text type="secondary">
        <Tooltip title={formatTime(status.last_fetch_at)}>
          <span>最近一次抓取 {fromNow(status.last_fetch_at)}</span>
        </Tooltip>
        {` · 每 ${status.fetch_interval_minutes} 分钟一轮 · 当前 ${status.feeds.filter((f) => f.enabled).length}/${status.feeds.length} 个源启用`}
      </Text>

      <Row gutter={[12, 12]}>
        {cards.map((card) => (
          <Col key={card.title} xs={12} sm={12} md={8} xl={4}>
            <Card size="small" styles={{ body: { padding: 16 } }}>
              <Statistic title={card.title} value={card.value} valueStyle={{ fontSize: 24 }} />
              <Text type="secondary" style={{ fontSize: 12 }}>
                {card.sub}
              </Text>
            </Card>
          </Col>
        ))}
      </Row>

      <Row gutter={[12, 12]}>
        <Col xs={24} lg={14}>
          <Card size="small" title="近 14 天新增条目" extra={<Text type="secondary">按发布时间统计</Text>}>
            <ResponsiveContainer width="100%" height={220}>
              <BarChart data={chartData} margin={{ top: 8, right: 8, left: -18, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke={token.colorBorderSecondary} vertical={false} />
                <XAxis dataKey="label" tick={{ fontSize: 11, fill: token.colorTextSecondary }} tickLine={false} axisLine={false} />
                <YAxis allowDecimals={false} tick={{ fontSize: 11, fill: token.colorTextSecondary }} tickLine={false} axisLine={false} />
                <ChartTooltip
                  contentStyle={chartTooltipStyle}
                  cursor={{ fill: token.colorFillQuaternary }}
                  labelFormatter={(label: string) => `日期 ${label}`}
                  formatter={(value: number) => [`${value} 条`, '新增']}
                />
                <Bar dataKey="count" fill={token.colorPrimary} radius={[4, 4, 0, 0]} maxBarSize={28} />
              </BarChart>
            </ResponsiveContainer>
          </Card>
        </Col>
        <Col xs={24} lg={10}>
          <Card
            size="small"
            title="评分分布"
            extra={<Text type="secondary">均分 {status.ratings.count ? status.ratings.avg.toFixed(2) : '—'}</Text>}
          >
            <ResponsiveContainer width="100%" height={220}>
              <BarChart data={ratingData} margin={{ top: 8, right: 8, left: -18, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke={token.colorBorderSecondary} vertical={false} />
                <XAxis dataKey="label" tick={{ fontSize: 11, fill: token.colorTextSecondary }} tickLine={false} axisLine={false} />
                <YAxis allowDecimals={false} tick={{ fontSize: 11, fill: token.colorTextSecondary }} tickLine={false} axisLine={false} />
                <ChartTooltip
                  contentStyle={chartTooltipStyle}
                  cursor={{ fill: token.colorFillQuaternary }}
                  formatter={(value: number) => [`${value} 次`, '评分']}
                />
                <Bar dataKey="count" radius={[4, 4, 0, 0]} maxBarSize={36}>
                  {ratingData.map((entry, index) => (
                    <Cell
                      key={entry.label}
                      fill={index >= 3 ? token.colorSuccess : index === 2 ? token.colorWarning : token.colorError}
                    />
                  ))}
                </Bar>
              </BarChart>
            </ResponsiveContainer>
          </Card>
        </Col>
      </Row>

      {failing.length > 0 && (
        <Card size="small" title={`抓取异常（${failing.length}）`}>
          <Space direction="vertical" size={6} style={{ width: '100%' }}>
            {failing.map((feed) => (
              <div key={feed.id} style={{ display: 'flex', gap: 12, alignItems: 'baseline' }}>
                <Text strong ellipsis style={{ maxWidth: 200 }}>
                  {feed.name}
                </Text>
                <Text type="secondary" style={{ fontSize: 12, whiteSpace: 'nowrap' }}>
                  {fromNow(feed.last_error_at)}
                </Text>
                <Tooltip title={feed.last_error}>
                  <Text type="danger" ellipsis style={{ fontSize: 12 }}>
                    {feed.last_error}
                  </Text>
                </Tooltip>
              </div>
            ))}
          </Space>
        </Card>
      )}
    </Space>
  );
}
