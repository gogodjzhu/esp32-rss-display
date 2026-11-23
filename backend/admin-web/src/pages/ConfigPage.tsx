import { useEffect, useState } from 'react';
import { Alert, App, Button, Card, Switch, Typography } from 'antd';
import { api } from '../api';
import type { AdminConfig } from '../types';

export function ConfigPage() {
  const { message } = App.useApp();
  const [config, setConfig] = useState<AdminConfig | null>(null);
  const [error, setError] = useState('');
  const [retry, setRetry] = useState(0);
  const [saving, setSaving] = useState(false);

  useEffect(() => {
    let active = true;
    setError('');
    api.getConfig().then(
      (value) => { if (active) setConfig(value); },
      (err: Error) => { if (active) setError(err.message); },
    );
    return () => { active = false; };
  }, [retry]);

  async function toggle(checked: boolean) {
    setSaving(true);
    try {
      setConfig(await api.updateConfig({ translation_enabled: checked }));
      message.success('配置已保存');
    } catch (err) {
      message.error(err instanceof Error ? err.message : String(err));
    } finally {
      setSaving(false);
    }
  }

  return (
    <Card size="small" title="标题翻译" loading={!config && !error}>
      {error ? (
        <Alert
          type="error"
          message={error}
          action={<Button size="small" onClick={() => setRetry(retry + 1)}>重试</Button>}
        />
      ) : (
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 16 }}>
          <div>
            <Typography.Text strong>翻译开关</Typography.Text>
            <div><Typography.Text type="secondary">使用公开翻译服务将英文标题译成中文，切换即保存。</Typography.Text></div>
          </div>
          <Switch
            checked={config?.translation_enabled ?? false}
            loading={saving}
            onChange={(checked) => void toggle(checked)}
            aria-label="标题翻译开关"
          />
        </div>
      )}
    </Card>
  );
}
