import { useMemo, useState } from 'react';
import {
  App,
  Avatar,
  Badge,
  Button,
  Card,
  Dropdown,
  Grid,
  Input,
  Space,
  Table,
  Tooltip,
  Typography,
} from 'antd';
import type { MenuProps } from 'antd';
import type { ColumnsType } from 'antd/es/table';
import {
  CheckCircleOutlined,
  DeleteOutlined,
  EditOutlined,
  MoreOutlined,
  PictureOutlined,
  PlusOutlined,
  ReloadOutlined,
  StopOutlined,
  UndoOutlined,
} from '@ant-design/icons';
import { api } from '../api';
import { coverURL, fromNow, formatTime, hostOf, isLater } from '../format';
import type { FeedStatus } from '../types';
import { FeedFormModal } from '../components/FeedFormModal';
import { FeedItemsDrawer } from '../components/FeedItemsDrawer';

const { Text } = Typography;

interface Props {
  feeds: FeedStatus[];
  loading: boolean;
  onChanged: () => void;
}

type Health = { status: 'success' | 'error' | 'default' | 'processing'; text: string };

function healthOf(feed: FeedStatus): Health {
  if (!feed.enabled) return { status: 'default', text: '停用' };
  if (feed.last_error_at && isLater(feed.last_error_at, feed.last_success_at)) {
    return { status: 'error', text: '异常' };
  }
  if (feed.last_success_at) return { status: 'success', text: '正常' };
  return { status: 'processing', text: '待抓取' };
}

export function FeedsPage({ feeds, loading, onChanged }: Props) {
  const { message, modal } = App.useApp();
  const screens = Grid.useBreakpoint();
  // 窄屏收窄名称列，让「条目」也能挤进首屏可视区域
  const narrow = !screens.md;
  const [keyword, setKeyword] = useState('');
  const [editing, setEditing] = useState<FeedStatus | null>(null);
  const [formOpen, setFormOpen] = useState(false);
  const [active, setActive] = useState<FeedStatus | null>(null);
  const [drawerOpen, setDrawerOpen] = useState(false);

  const filtered = useMemo(() => {
    const q = keyword.trim().toLowerCase();
    if (!q) return feeds;
    return feeds.filter(
      (f) => f.name.toLowerCase().includes(q) || f.url.toLowerCase().includes(q),
    );
  }, [feeds, keyword]);

  function openCreate() {
    setEditing(null);
    setFormOpen(true);
  }

  function openEdit(feed: FeedStatus) {
    setEditing(feed);
    setFormOpen(true);
  }

  function openItems(feed: FeedStatus) {
    setActive(feed);
    setDrawerOpen(true);
  }

  async function toggle(feed: FeedStatus) {
    try {
      await api.updateFeed(feed.id, { enabled: !feed.enabled });
      message.success(feed.enabled ? '已停用' : '已启用');
      onChanged();
    } catch (err) {
      message.error(err instanceof Error ? err.message : String(err));
    }
  }

  async function clearCover(feed: FeedStatus) {
    try {
      await api.clearCover(feed.id);
      message.success('已恢复按名称生成的默认封面');
      onChanged();
    } catch (err) {
      message.error(err instanceof Error ? err.message : String(err));
    }
  }

  function confirmDelete(feed: FeedStatus) {
    modal.confirm({
      title: `删除「${feed.name}」？`,
      content: '该源下已抓取的条目、评分与曝光记录会一并删除，无法恢复。',
      okText: '删除',
      okButtonProps: { danger: true },
      cancelText: '取消',
      onOk: async () => {
        try {
          await api.deleteFeed(feed.id);
          message.success('已删除');
          onChanged();
        } catch (err) {
          // 抛出让 antd 保持弹窗打开，用户可以重试
          message.error(err instanceof Error ? err.message : String(err));
          throw err;
        }
      },
    });
  }

  const columns: ColumnsType<FeedStatus> = [
    {
      title: '订阅源',
      dataIndex: 'name',
      key: 'name',
      fixed: 'left',
      width: narrow ? 132 : 230,
      render: (_, feed) => (
        <Space size={10} style={{ width: '100%' }} align="center">
          <Avatar
            shape="square"
            size={narrow ? 30 : 38}
            src={coverURL(feed)}
            icon={<PictureOutlined />}
            style={{ flex: 'none' }}
          />
          <div style={{ minWidth: 0 }}>
            <Text strong ellipsis style={{ display: 'block', maxWidth: narrow ? 84 : 168 }}>
              {feed.name}
            </Text>
            <Tooltip title={feed.url}>
              <Text
                type="secondary"
                ellipsis
                style={{ fontSize: 12, display: 'block', maxWidth: narrow ? 84 : 168 }}
              >
                {hostOf(feed.url)}
              </Text>
            </Tooltip>
          </div>
        </Space>
      ),
    },
    {
      title: '状态',
      key: 'health',
      width: narrow ? 68 : 82,
      render: (_, feed) => {
        const health = healthOf(feed);
        return <Badge status={health.status} text={health.text} />;
      },
    },
    {
      title: '最近成功',
      key: 'last_success',
      width: 104,
      responsive: ['md'],
      render: (_, feed) => (
        <Tooltip title={formatTime(feed.last_success_at)}>
          <Text type="secondary">{fromNow(feed.last_success_at)}</Text>
        </Tooltip>
      ),
    },
    {
      title: '最近新内容',
      key: 'last_new',
      width: 108,
      responsive: ['lg'],
      render: (_, feed) => (
        <Tooltip title={formatTime(feed.last_new_item_at)}>
          <Text type="secondary">{fromNow(feed.last_new_item_at)}</Text>
        </Tooltip>
      ),
    },
    {
      title: '条目',
      key: 'items',
      width: 104,
      align: 'right',
      render: (_, feed) => (
        <Tooltip title={`今日新增 ${feed.items_today} 条 / 共 ${feed.items_total} 条`}>
          <span>
            <Text strong>{feed.items_total}</Text>
            <Text type="secondary" style={{ fontSize: 12 }}>
              {' '}
              · {feed.items_today}
            </Text>
          </span>
        </Tooltip>
      ),
    },
    {
      title: '互动',
      key: 'engagement',
      width: 168,
      responsive: ['xl'],
      render: (_, feed) => (
        <Tooltip title={`展示 ${feed.shown_total} 次 · NFC 打开 ${feed.nfc_opens} 次 · 平均评分 ${feed.rating_count ? feed.rating_avg.toFixed(2) : '无'}`}>
          <Text type="secondary" style={{ fontSize: 12, whiteSpace: 'nowrap' }}>
            展示 <Text>{feed.shown_total}</Text> · NFC <Text>{feed.nfc_opens}</Text> · ★{' '}
            <Text>{feed.rating_count ? feed.rating_avg.toFixed(1) : '—'}</Text>
          </Text>
        </Tooltip>
      ),
    },
    {
      title: '最近错误',
      key: 'error',
      // 不设宽度：作为弹性列吃掉表格剩余空间，避免右侧留白
      responsive: ['lg'],
      render: (_, feed) =>
        feed.last_error ? (
          <Tooltip title={feed.last_error}>
            <Text type="danger" ellipsis style={{ maxWidth: 140, fontSize: 12 }}>
              {feed.last_error}
            </Text>
          </Tooltip>
        ) : (
          <Text type="secondary">—</Text>
        ),
    },
    {
      title: '',
      key: 'actions',
      width: 52,
      align: 'right',
      fixed: 'right',
      render: (_, feed) => {
        const items: MenuProps['items'] = [
          { key: 'edit', icon: <EditOutlined />, label: '编辑' },
          {
            key: 'toggle',
            icon: feed.enabled ? <StopOutlined /> : <CheckCircleOutlined />,
            label: feed.enabled ? '停用' : '启用',
          },
          {
            key: 'items',
            icon: <PictureOutlined />,
            label: '查看内容',
          },
          ...(feed.cover_image_path
            ? [{ key: 'cover-clear', icon: <UndoOutlined />, label: '恢复默认封面' }]
            : []),
          { type: 'divider' as const },
          { key: 'delete', icon: <DeleteOutlined />, label: '删除', danger: true },
        ];

        return (
          <div onClick={(event) => event.stopPropagation()}>
            <Dropdown
              trigger={['click']}
              menu={{
                items,
                onClick: ({ key }) => {
                  if (key === 'edit') openEdit(feed);
                  if (key === 'toggle') void toggle(feed);
                  if (key === 'items') openItems(feed);
                  if (key === 'cover-clear') void clearCover(feed);
                  if (key === 'delete') confirmDelete(feed);
                },
              }}
            >
              <Button type="text" icon={<MoreOutlined />} aria-label="操作" />
            </Dropdown>
          </div>
        );
      },
    },
  ];

  const enabledCount = feeds.filter((f) => f.enabled).length;

  return (
    <Space direction="vertical" size={12} style={{ width: '100%' }}>
      <Space wrap style={{ width: '100%', justifyContent: 'space-between' }}>
        <Space wrap>
          <Button type="primary" icon={<PlusOutlined />} onClick={openCreate}>
            新增订阅源
          </Button>
          <Input.Search
            allowClear
            placeholder="搜索名称或地址"
            style={{ width: 220 }}
            value={keyword}
            onChange={(event) => setKeyword(event.target.value)}
          />
        </Space>
        <Text type="secondary">
          共 {feeds.length} 个源 · 启用 {enabledCount} 个
        </Text>
      </Space>

      <Card size="small" styles={{ body: { padding: 0 } }}>
        <Table<FeedStatus>
          rowKey="id"
          size="middle"
          loading={loading}
          columns={columns}
          dataSource={filtered}
          pagination={false}
          scroll={{ x: 'max-content' }}
          locale={{ emptyText: feeds.length ? '没有匹配的订阅源' : '还没有订阅源，点「新增订阅源」添加' }}
          onRow={(feed) => ({
            onClick: () => openItems(feed),
            style: { cursor: 'pointer' },
          })}
        />
      </Card>

      <Text type="secondary" style={{ fontSize: 12 }}>
        <ReloadOutlined /> 改动在下一轮抓取生效；点击任意一行可查看该源已抓取的条目。
      </Text>

      <FeedFormModal
        open={formOpen}
        feed={editing}
        onCancel={() => setFormOpen(false)}
        onSaved={() => {
          setFormOpen(false);
          onChanged();
        }}
      />

      <FeedItemsDrawer
        open={drawerOpen}
        feed={active}
        onClose={() => setDrawerOpen(false)}
      />
    </Space>
  );
}
