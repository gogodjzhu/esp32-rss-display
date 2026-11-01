import { useCallback, useEffect, useState } from 'react';
import {
  Avatar,
  Button,
  Drawer,
  Empty,
  Grid,
  List,
  Space,
  Spin,
  Tooltip,
  Typography,
  theme as antdTheme,
} from 'antd';
import { LinkOutlined, PictureOutlined } from '@ant-design/icons';
import { api } from '../api';
import { fromNow, formatTime } from '../format';
import type { AdminItem, FeedStatus } from '../types';

const { Text, Link } = Typography;
const PAGE_SIZE = 30;

interface Props {
  feed: FeedStatus | null;
  open: boolean;
  onClose: () => void;
}

/** 单个订阅源的条目抽屉：标题 + 原文跳转 + 互动数据，移动端全屏。 */
export function FeedItemsDrawer({ feed, open, onClose }: Props) {
  const { token } = antdTheme.useToken();
  const screens = Grid.useBreakpoint();

  const [items, setItems] = useState<AdminItem[]>([]);
  const [total, setTotal] = useState(0);
  const [hasMore, setHasMore] = useState(false);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');

  const load = useCallback(
    async (offset: number) => {
      if (!feed) return;
      setLoading(true);
      setError('');
      try {
        const page = await api.feedItems(feed.id, offset, PAGE_SIZE);
        setItems((prev) => (offset === 0 ? page.items : [...prev, ...page.items]));
        setTotal(page.total);
        setHasMore(page.has_more);
      } catch (err) {
        setError(err instanceof Error ? err.message : String(err));
      } finally {
        setLoading(false);
      }
    },
    [feed],
  );

  useEffect(() => {
    if (!open || !feed) return;
    setItems([]);
    setTotal(0);
    setHasMore(false);
    void load(0);
  }, [open, feed, load]);

  return (
    <Drawer
      open={open}
      onClose={onClose}
      width={screens.md ? 720 : '100%'}
      title={
        <Space direction="vertical" size={0}>
          <Text strong>{feed?.name ?? ''}</Text>
          <Text type="secondary" style={{ fontSize: 12, fontWeight: 'normal' }}>
            共 {total} 条 · 最近新增 {fromNow(feed?.last_new_item_at)}
          </Text>
        </Space>
      }
      extra={
        feed && (
          <Tooltip title="打开订阅源原文">
            <Button type="text" icon={<LinkOutlined />} href={feed.url} target="_blank" />
          </Tooltip>
        )
      }
    >
      {error && (
        <Empty
          description={error}
          image={Empty.PRESENTED_IMAGE_SIMPLE}
          style={{ marginTop: 48 }}
        />
      )}

      {!error && !loading && items.length === 0 && (
        <Empty description="该源还没有抓取到条目" style={{ marginTop: 48 }} />
      )}

      <List
        itemLayout="horizontal"
        dataSource={items}
        renderItem={(item) => (
          <List.Item
            key={item.id}
            style={{ paddingInline: 0 }}
            actions={[
              <Tooltip key="open" title="跳转原文">
                <Button
                  type="text"
                  size="small"
                  icon={<LinkOutlined />}
                  href={item.url}
                  target="_blank"
                />
              </Tooltip>,
            ]}
          >
            <List.Item.Meta
              avatar={
                <Avatar
                  shape="square"
                  size={56}
                  src={item.image_url}
                  icon={<PictureOutlined />}
                  style={{ border: `1px solid ${token.colorBorderSecondary}` }}
                />
              }
              title={
                <Link href={item.url} target="_blank" ellipsis>
                  {item.title || '（无标题）'}
                </Link>
              }
              description={
                <Space size={12} wrap style={{ fontSize: 12 }}>
                  <Tooltip title={formatTime(item.published_at || item.created_at)}>
                    <span>{fromNow(item.published_at || item.created_at)}</span>
                  </Tooltip>
                  <span>展示 {item.show_count}</span>
                  <span>NFC {item.nfc_opens}</span>
                  {item.rating_count > 0 && (
                    <span>
                      ★ {item.rating_avg.toFixed(1)}（{item.rating_count}）
                    </span>
                  )}
                </Space>
              }
            />
          </List.Item>
        )}
      />

      <div style={{ textAlign: 'center', marginTop: 16 }}>
        {loading && <Spin />}
        {!loading && hasMore && (
          <Button onClick={() => void load(items.length)}>加载更多（剩余 {total - items.length}）</Button>
        )}
        {!loading && !hasMore && items.length > 0 && <Text type="secondary">已全部加载</Text>}
      </div>
    </Drawer>
  );
}
