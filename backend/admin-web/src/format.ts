import dayjs from 'dayjs';
import relativeTime from 'dayjs/plugin/relativeTime';
import 'dayjs/locale/zh-cn';

dayjs.extend(relativeTime);
dayjs.locale('zh-cn');

const ABSOLUTE = 'YYYY-MM-DD HH:mm:ss';

/** 把后端时间串解析成 dayjs；缺失或落在 1970 之前（Go 零值）都视为无效。 */
function parse(value?: string | null) {
  if (!value) return null;
  const d = dayjs(value);
  if (!d.isValid() || d.year() < 1970) return null;
  return d;
}

/** 绝对时间，缺失时返回破折号。表格 Tooltip 使用。 */
export function formatTime(value?: string | null): string {
  const d = parse(value);
  return d ? d.format(ABSOLUTE) : '—';
}

/** 相对时间（如「3 分钟前」），用于表格主展示，悬停再看绝对时间。 */
export function fromNow(value?: string | null): string {
  const d = parse(value);
  return d ? d.fromNow() : '—';
}

/** 人可读的字节数。 */
export function formatBytes(bytes: number): string {
  if (!bytes) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  let value = bytes;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
}

/** 表格里只展示主机名，完整地址放到 Tooltip / 链接里。 */
export function hostOf(raw: string): string {
  try {
    return new URL(raw).host;
  } catch {
    return raw;
  }
}

/** 与后端 validateFeedURL 同源的前端预校验，避免明显非法的请求打到后端。 */
export function checkFeedURL(raw: string): string {
  const value = raw.trim();
  if (!value) return 'URL 必填';
  let url: URL;
  try {
    url = new URL(value);
  } catch {
    return 'URL 不是合法的链接';
  }
  if (url.protocol !== 'http:' && url.protocol !== 'https:') return 'URL 只支持 http/https';
  if (!url.hostname) return 'URL 缺少主机名';
  return '';
}

/** 判断 a 是否晚于 b（任一侧缺失时视为不晚于）。 */
export function isLater(a?: string | null, b?: string | null): boolean {
  const left = parse(a);
  if (!left) return false;
  const right = parse(b);
  return right ? left.isAfter(right) : true;
}

/** 订阅源封面的 URL；用名称/自定义封面做 cache buster。 */
export function coverURL(feed: { id: number; name: string; cover_image_path: string }): string {
  const version = feed.cover_image_path || feed.name || '';
  return `/image/feed/${feed.id}.jpg?v=${encodeURIComponent(version)}`;
}
