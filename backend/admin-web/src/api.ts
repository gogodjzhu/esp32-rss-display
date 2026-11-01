import type {
  AdminItemList,
  AdminStatus,
  FeedInput,
  FeedPreview,
  FeedStatus,
} from './types';

// 后端错误统一返回 {error}，这里还原成可直接展示的文案。
async function parse<T>(res: Response): Promise<T> {
  if (res.status === 204) return null as T;
  const text = await res.text();
  let data: unknown = null;
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      data = null;
    }
  }
  if (!res.ok) {
    const message =
      data && typeof data === 'object' && 'error' in data
        ? String((data as { error: unknown }).error)
        : `请求失败（HTTP ${res.status}）`;
    throw new Error(message);
  }
  return data as T;
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  return parse<T>(await fetch(path, init));
}

function jsonRequest<T>(path: string, method: string, body: unknown): Promise<T> {
  return request<T>(path, {
    method,
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
}

export const api = {
  status: () => request<AdminStatus>('/admin/status'),

  preview: (url: string) => jsonRequest<FeedPreview>('/admin/feeds/preview', 'POST', { url }),

  feedItems: (id: number, offset = 0, limit = 30) =>
    request<AdminItemList>(`/admin/feeds/${id}/items?offset=${offset}&limit=${limit}`),

  createFeed: (input: FeedInput) => jsonRequest<FeedStatus>('/v1/feeds', 'POST', input),

  updateFeed: (id: number, patch: Partial<FeedInput>) =>
    jsonRequest<FeedStatus>(`/v1/feeds/${id}`, 'PATCH', patch),

  deleteFeed: (id: number) => request<void>(`/v1/feeds/${id}`, { method: 'DELETE' }),

  uploadCover: async (id: number, file: File) => {
    const form = new FormData();
    form.append('image', file);
    return request<FeedStatus>(`/v1/feeds/${id}/cover`, { method: 'POST', body: form });
  },

  clearCover: (id: number) =>
    request<FeedStatus>(`/v1/feeds/${id}/cover`, { method: 'DELETE' }),
};
