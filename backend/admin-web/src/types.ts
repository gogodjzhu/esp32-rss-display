// 与后端 /admin/status、/admin/feeds/{id}/items、/admin/feeds/preview 的响应保持一致。
// 后端是唯一数据源，这里只做类型声明。

export interface FeedStatus {
  id: number;
  name: string;
  url: string;
  enabled: boolean;
  cover_image_path: string;
  items_today: number;
  items_total: number;
  shown_total: number;
  nfc_opens: number;
  rating_count: number;
  rating_avg: number;
  last_success_at: string | null;
  last_error_at: string | null;
  last_error?: string;
  last_new_item_at: string | null;
}

export interface DailyCount {
  date: string;
  count: number;
}

export interface RatingStats {
  count: number;
  avg: number;
  dist: number[];
}

export interface AdminStatus {
  time: string;
  max_age_hours: number;
  fetch_interval_minutes: number;
  last_fetch_at: string | null;
  feeds: FeedStatus[];
  items: { total: number; last_24h: number };
  devices: { total: number; active_1h: number };
  images: { files: number; bytes: number };
  database: { file?: string; bytes: number };
  ratings: RatingStats;
  items_by_day: DailyCount[];
}

export interface AdminItem {
  id: number;
  title: string;
  url: string;
  image_url: string;
  published_at: string | null;
  created_at: string;
  show_count: number;
  nfc_opens: number;
  rating_count: number;
  rating_avg: number;
}

export interface AdminItemList {
  items: AdminItem[];
  total: number;
  has_more: boolean;
}

export interface PreviewItem {
  title: string;
  url: string;
  published_at: string | null;
}

export interface FeedPreview {
  url: string;
  title: string;
  description: string;
  items: PreviewItem[];
  item_count: number;
}

export interface FeedInput {
  name: string;
  url: string;
  enabled: boolean;
}
