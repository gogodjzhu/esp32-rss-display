import { useEffect, useState } from 'react';
import { Alert, App, Card, Space, Switch, Tag, Typography } from 'antd';
import { TranslationOutlined } from '@ant-design/icons';
import { api } from '../api';

const { Text, Paragraph } = Typography;

/**
 * 「配置」页：在线修改后端运行期配置。
 * 开关切换即保存，配置保存在数据库，重启后仍然生效。
 */
export function ConfigPage() {
  const { message } = App.useApp();
  const [enabled, setEnabled] = useState(false);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    api
      .config()
      .then((cfg) => alive && setEnabled(cfg.translation_enabled))
      .catch((err) => alive && setError(err instanceof Error ? err.message : String(err)))
      .finally(() => alive && setLoading(false));
    return () => {
      alive = false;
    };
  }, []);

  async function toggle(next: boolean) {
    setEnabled(next);
    setSaving(true);
    setError(null);
    try {
      const cfg = await api.updateConfig({ translation_enabled: next });
      setEnabled(cfg.translation_enabled);
      message.success(next ? '已开启标题翻译' : '已关闭标题翻译');
    } catch (err) {
      setEnabled(!next);
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setSaving(false);
    }
  }

  return (
    <Space direction="vertical" size={16} style={{ width: '100%', maxWidth: 960 }}>
      {error && <Alert type="error" showIcon message="配置加载或保存失败" description={error} />}

      <Card loading={loading} title="基础配置" extra={<Tag>{saving ? '保存中' : '自动保存'}</Tag>}>
        <Space size={12} align="start">
          <TranslationOutlined style={{ fontSize: 20, marginTop: 2 }} />
          <div>
            <Space size={10} align="center" wrap>
              <Text strong style={{ fontSize: 15 }}>
                RSS 标题英文 → 中文自动翻译
              </Text>
              <Switch
                checked={enabled}
                onChange={(next) => void toggle(next)}
                loading={saving}
                checkedChildren="开启"
                unCheckedChildren="关闭"
                aria-label="RSS 标题自动翻译开关"
              />
            </Space>
            <Paragraph type="secondary" style={{ margin: '8px 0 0', maxWidth: 680 }}>
              开启后，后台会调用公开翻译接口把英文条目标题转换为中文，设备端 next 接口返回译文；
              尚未翻译或接口失败时使用原标题。翻译在后台分批进行，已有条目需要一点时间补齐。
            </Paragraph>
          </div>
        </Space>
      </Card>
    </Space>
  );
}
