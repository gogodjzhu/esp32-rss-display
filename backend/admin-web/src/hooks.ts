import { useCallback, useEffect, useRef, useState } from 'react';
import { api } from './api';
import type { AdminStatus } from './types';

export interface StatusState {
  status: AdminStatus | null;
  loading: boolean;
  error: string | null;
  reload: (silent?: boolean) => Promise<void>;
}

/** 拉取 /admin/status。silent 为 true 时不显示 loading（自动刷新用）。 */
export function useAdminStatus(): StatusState {
  const [status, setStatus] = useState<AdminStatus | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(true);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  const reload = useCallback(async (silent = false) => {
    if (!silent) setLoading(true);
    try {
      const next = await api.status();
      if (mounted.current) {
        setStatus(next);
        setError(null);
      }
    } catch (err) {
      if (mounted.current) setError(err instanceof Error ? err.message : String(err));
    } finally {
      if (mounted.current) setLoading(false);
    }
  }, []);

  useEffect(() => {
    void reload();
  }, [reload]);

  return { status, loading, error, reload };
}

/** 定时静默刷新；disabled 时停止。 */
export function useAutoRefresh(enabled: boolean, intervalMs: number, run: () => void) {
  const saved = useRef(run);
  useEffect(() => {
    saved.current = run;
  }, [run]);

  useEffect(() => {
    if (!enabled || intervalMs <= 0) return;
    const timer = window.setInterval(() => {
      if (document.visibilityState === 'visible') saved.current();
    }, intervalMs);
    return () => window.clearInterval(timer);
  }, [enabled, intervalMs]);
}
