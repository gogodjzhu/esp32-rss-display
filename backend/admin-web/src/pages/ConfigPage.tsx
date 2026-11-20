import { useCallback, useEffect, useState } from 'react';
import { Alert, App, Button, Card, Divider, Space, Switch, Tag, Typography } from 'antd';
import { TranslationOutlined } from '@ant-design/icons';
import { api } from '../api';

const { Text, Paragraph } = Typography;

/**
 * 「配置」页：在线修改后端运行期配置。
 * 配置保存在数据库，保存后立即生效，不需要重启服务。
 */
export function ConfigPage() {
  const { message } = App.useApp();
  const [enabled, setEnabled] = useState(false);
  const [saved, setSaved] = useState(false);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const cfg = await api.config();
      setEnabled(cfg.translation_enabled);
      setSaved(cfg.translation_enabled);
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void load();
  }, [load]);

  const dirty = enabled !== saved;

  async function save() {
    setSaving(true);
    setError(null);
    try {
      const cfg = await api.updateConfig({ translation_enabled: enabled });
      setEnabled(cfg.translation_enabled);
      setSaved(cfg.translation_enabled);
      message.success('配置已保存，翻译会在后台逐步生效');
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setSaving(false);
    }
  }

  return (
    <Space direction="vertical" size={16} style={{ width: '100%', maxWidth: 960 }}>
      {error && <Alert type="error" showIcon message="配置加载或保存失败" description={error} />}

      <Card
        loading={loading}
        title="基础配置"
        extra={dirty ? <Tag color="warning">有未保存的修改</Tag> : <Tag>已保存</Tag>}
      >
        <Space direction="vertical" size={16} style={{ width: '100%' }}>
          <Space size={12} align="start">
            <TranslationOutlined style={{ fontSize: 20, marginTop: 2 }} />
            <div>
              <Space size={10} align="center" wrap>
                <Text strong style={{ fontSize: 15 }}>
                  RSS 标题英文 → 中文自动翻译
                </Text>
                <Switch
                  checked={enabled}
                  onChange={setEnabled}
                  checkedChildren="开启"
                  unCheckedChildren="关闭"
                  aria-label="RSS 标题自动翻译开关"
                />
              </Space>
              <Paragraph type="secondary" style={{ margin: '8px 0 0', maxWidth: 680 }}>
                开启后，后台会调用公开翻译接口把英文条目标题转换为中文，设备端 next 接口返回译文；
                尚未翻译或接口失败时自动使用原标题。翻译在后台分批进行，已有条目需要一点时间补齐。
              </Paragraph>
            </div>
          </Space>

          <Divider style={{ margin: 0 }} />

          <Space direction="vertical" size={8}>
            <Text type="secondary">配置保存在数据库，保存后立即生效，重启服务不会丢失。</Text>
            <Text type="secondary">使用免 Key 的公开翻译接口，接口不可用时保留原标题，不影响内容展示。</Text>
            <Button type="primary" onClick={() => void save()} loading={saving} disabled={!dirty}>
              保存配置
            </Button>
          </Space>
        </Space>
      </Card>
    </Space>
  );
}
