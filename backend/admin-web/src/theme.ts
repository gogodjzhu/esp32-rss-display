import { useEffect, useState } from 'react';

export type ThemeMode = 'auto' | 'light' | 'dark';

const STORAGE_KEY = 'esp32-rss-admin-theme';

function readStored(): ThemeMode {
  const value = localStorage.getItem(STORAGE_KEY);
  return value === 'light' || value === 'dark' || value === 'auto' ? value : 'auto';
}

/** 订阅系统深色偏好；不支持时按浅色处理。 */
function usePrefersDark(): boolean {
  const query = '(prefers-color-scheme: dark)';
  const [dark, setDark] = useState(() => window.matchMedia?.(query).matches ?? false);

  useEffect(() => {
    const media = window.matchMedia?.(query);
    if (!media) return;
    const onChange = (event: MediaQueryListEvent) => setDark(event.matches);
    media.addEventListener('change', onChange);
    return () => media.removeEventListener('change', onChange);
  }, []);

  return dark;
}

/** 主题模式：auto 跟随系统，light/dark 显式覆盖，选择会持久化。 */
export function useThemeMode() {
  const [mode, setMode] = useState<ThemeMode>(readStored);
  const prefersDark = usePrefersDark();

  useEffect(() => {
    localStorage.setItem(STORAGE_KEY, mode);
  }, [mode]);

  const dark = mode === 'auto' ? prefersDark : mode === 'dark';
  return { mode, setMode, dark };
}
