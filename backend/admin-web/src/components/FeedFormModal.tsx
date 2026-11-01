import { useEffect, useMemo, useState } from 'react';
import {
  Alert,
  App,
  Button,
  Card,
  Col,
  Divider,
  Form,
  Input,
  List,
  Modal,
  Row,
  Space,
  Switch,
  Typography,
  Upload,
} from 'antd';
import type { UploadFile } from 'antd';
import { InboxOutlined, LinkOutlined } from '@ant-design/icons';
import { api } from '../api';
import { checkFeedURL, fromNow } from '../format';
import type { FeedPreview, FeedStatus } from '../types';

const { Text } = Typography;

interface Props {
  open: boolean;
  feed: FeedStatus | null;
  onCancel: () => void;
  onSaved: () => void;
}

interface FormValues {
  name: string;
  url: string;
  enabled: boolean;
  cover?: UploadFile[];
}

/** 新增/编辑订阅源。新增时必须先「校验预览」成功才能保存，避免坏源入库。 */
export function FeedFormModal({ open, feed, onCancel, onSaved }: Props) {
  const { message } = App.useApp();
  const [form] = Form.useForm<FormValues>();
  const isNew = !feed;

  // 用 state 跟随 URL 输入，避免 useWatch 在弹窗关闭（Form 未挂载）时告警。
  const [url, setUrl] = useState('');
  const [preview, setPreview] = useState<FeedPreview | null>(null);
  const [previewError, setPreviewError] = useState('');
  const [previewing, setPreviewing] = useState(false);
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    if (!open) return;
    form.setFieldsValue({
      name: feed?.name ?? '',
      url: feed?.url ?? '',
      enabled: feed?.enabled ?? true,
      cover: [],
    });
    setUrl(feed?.url ?? '');
    setPreview(null);
    setPreviewError('');
  }, [open, feed, form]);

  // URL 一变，之前的预览结果就不再对应当前输入。
  useEffect(() => {
    setPreview(null);
    setPreviewError('');
  }, [url]);

  const canSubmit = useMemo(() => {
    if (!isNew) return true;
    return Boolean(preview && url && preview.url === url.trim());
  }, [isNew, preview, url]);

  async function handlePreview() {
    const error = checkFeedURL(url);
    if (error) {
      setPreview(null);
      setPreviewError(error);
      return;
    }
    setPreviewing(true);
    setPreviewError('');
    try {
      const result = await api.preview(url.trim());
      setPreview(result);
      // 名称留空时用源标题兜底，省一次输入
      if (!form.getFieldValue('name')) form.setFieldValue('name', result.title);
      if (!result.items.length) {
        message.warning('该源当前没有条目，可能刚上线或内容为空');
      }
    } catch (err) {
      setPreview(null);
      setPreviewError(err instanceof Error ? err.message : String(err));
    } finally {
      setPreviewing(false);
    }
  }

  async function handleSubmit() {
    let values: FormValues;
    try {
      values = await form.validateFields();
    } catch {
      return;
    }

    setSaving(true);
    try {
      const payload = {
        name: values.name.trim(),
        url: values.url.trim(),
        enabled: values.enabled,
      };
      const saved = feed
        ? await api.updateFeed(feed.id, payload)
        : await api.createFeed(payload);

      const file = values.cover?.[0]?.originFileObj;
      if (file) {
        try {
          await api.uploadCover(saved.id, file);
        } catch (err) {
          message.warning(`订阅源已保存，但封面上传失败：${err instanceof Error ? err.message : String(err)}`);
          onSaved();
          return;
        }
      }

      message.success(feed ? '已保存' : '已新增，下一轮抓取生效');
      onSaved();
    } catch (err) {
      message.error(err instanceof Error ? err.message : String(err));
    } finally {
      setSaving(false);
    }
  }

  return (
    <Modal
      title={isNew ? '新增订阅源' : `编辑订阅源 · ${feed?.name ?? ''}`}
      open={open}
      onCancel={onCancel}
      width={720}
      destroyOnClose
      footer={[
        <Button key="cancel" onClick={onCancel}>
          取消
        </Button>,
        <Button key="preview" icon={<LinkOutlined />} loading={previewing} onClick={handlePreview}>
          校验并预览
        </Button>,
        <Button
          key="save"
          type="primary"
          loading={saving}
          disabled={!canSubmit}
          onClick={handleSubmit}
        >
          保存
        </Button>,
      ]}
    >
      <Form
        form={form}
        layout="vertical"
        requiredMark={false}
        onValuesChange={(changed: Partial<FormValues>) => {
          // URL 变化由下面的 effect 统一清空预览结果
          if ('url' in changed) setUrl(changed.url ?? '');
        }}
      >
        <Row gutter={16}>
          <Col xs={24} sm={10}>
            <Form.Item
              name="name"
              label="名称"
              rules={[{ required: true, message: '名称必填' }]}
            >
              <Input placeholder="例如 HackerNews" allowClear />
            </Form.Item>
          </Col>
          <Col xs={24} sm={14}>
            <Form.Item
              name="url"
              label="订阅源 URL"
              rules={[{ required: true, message: 'URL 必填' }]}
            >
              <Input placeholder="https://example.com/rss.xml" allowClear />
            </Form.Item>
          </Col>
        </Row>

        <Row gutter={16} align="top">
          <Col xs={24} sm={14}>
            <Form.Item label="封面（可选，JPEG/PNG）" name="cover" valuePropName="fileList" getValueFromEvent={normFile}>
              <Upload.Dragger
                accept="image/jpeg,image/png"
                maxCount={1}
                beforeUpload={() => false}
              >
                <p className="ant-upload-drag-icon" style={{ marginBottom: 0 }}>
                  <InboxOutlined />
                </p>
                <Text type="secondary" style={{ fontSize: 12 }}>
                  点击或拖拽上传，保存时自动裁剪为 320×240
                </Text>
              </Upload.Dragger>
            </Form.Item>
          </Col>
          <Col xs={24} sm={10}>
            <Form.Item label="状态" name="enabled" valuePropName="checked">
              <Switch checkedChildren="启用" unCheckedChildren="停用" />
            </Form.Item>
            {isNew && (
              <Text type="secondary" style={{ fontSize: 12 }}>
                新增时会真实抓取一次校验，保存可能需要几秒。
              </Text>
            )}
          </Col>
        </Row>

        <Divider style={{ margin: '8px 0 12px' }} />

        {previewError && (
          <Alert type="error" showIcon message="校验失败" description={previewError} style={{ marginBottom: 12 }} />
        )}

        {preview && (
          <Card
            size="small"
            title={
              <Space>
                <Text strong>{preview.title}</Text>
                <Text type="secondary" style={{ fontSize: 12 }}>
                  解析到 {preview.item_count} 条内容
                </Text>
              </Space>
            }
          >
            {preview.description && (
              <Text type="secondary" style={{ fontSize: 12, display: 'block', marginBottom: 8 }}>
                {preview.description}
              </Text>
            )}
            <List
              size="small"
              dataSource={preview.items}
              locale={{ emptyText: '该源当前没有条目' }}
              renderItem={(item) => (
                <List.Item style={{ paddingInline: 0 }}>
                  <Space direction="vertical" size={0} style={{ width: '100%' }}>
                    <a href={item.url} target="_blank" rel="noreferrer">
                      {item.title || item.url}
                    </a>
                    <Text type="secondary" style={{ fontSize: 12 }}>
                      {item.published_at ? fromNow(item.published_at) : '无发布时间'}
                    </Text>
                  </Space>
                </List.Item>
              )}
            />
            {preview.item_count > preview.items.length && (
              <Text type="secondary" style={{ fontSize: 12 }}>
                仅展示前 {preview.items.length} 条
              </Text>
            )}
          </Card>
        )}

        {!preview && !previewError && isNew && (
          <Alert
            type="info"
            showIcon
            message="先点「校验并预览」，确认能解析出条目后再保存"
          />
        )}
      </Form>
    </Modal>
  );
}

function normFile(event: { fileList: UploadFile[] } | UploadFile[]) {
  return Array.isArray(event) ? event : event.fileList;
}
