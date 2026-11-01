package api

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/esp32-rss-display/backend/server/cover"
	"github.com/esp32-rss-display/backend/server/database"
	"github.com/esp32-rss-display/backend/server/models"
	"gorm.io/gorm"
)

// 订阅源 CRUD：config.yaml 只做首次种子，日常增删改走接口。
// 抓取循环每轮都从库里读 feeds，改动下一轮即生效。

type feedRequest struct {
	Name    *string `json:"name"`
	URL     *string `json:"url"`
	Enabled *bool   `json:"enabled"`
}

// 自定义封面单文件上限；内容会重新编码为 320×240 JPEG，不需要保留原图。
const maxCoverUpload = 8 << 20

func decodeFeedRequest(r *http.Request) (*feedRequest, error) {
	body, err := io.ReadAll(io.LimitReader(r.Body, 1<<16))
	if err != nil {
		return nil, err
	}
	var req feedRequest
	dec := json.NewDecoder(strings.NewReader(string(body)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil {
		return nil, err
	}
	return &req, nil
}

func validateFeedURL(raw string) error {
	u, err := url.Parse(raw)
	if err != nil {
		return errors.New("url 不是合法的链接")
	}
	if u.Scheme != "http" && u.Scheme != "https" {
		return errors.New("url 只支持 http/https")
	}
	if u.Host == "" {
		return errors.New("url 缺少主机名")
	}
	return nil
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

func ListFeeds(w http.ResponseWriter, r *http.Request) {
	var feeds []models.Feed
	if err := database.GetDB().Order("id ASC").Find(&feeds).Error; err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, feeds)
}

func CreateFeed(w http.ResponseWriter, r *http.Request) {
	req, err := decodeFeedRequest(r)
	if err != nil {
		writeError(w, http.StatusBadRequest, "请求体格式错误: "+err.Error())
		return
	}
	if req.Name == nil || strings.TrimSpace(*req.Name) == "" {
		writeError(w, http.StatusBadRequest, "name 必填")
		return
	}
	if req.URL == nil || strings.TrimSpace(*req.URL) == "" {
		writeError(w, http.StatusBadRequest, "url 必填")
		return
	}
	if err := validateFeedURL(*req.URL); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	feed := models.Feed{Name: strings.TrimSpace(*req.Name), URL: strings.TrimSpace(*req.URL), Enabled: true}
	if req.Enabled != nil {
		feed.Enabled = *req.Enabled
	}

	db := database.GetDB()
	var existing models.Feed
	if err := db.Where("url = ?", feed.URL).First(&existing).Error; err == nil {
		writeError(w, http.StatusConflict, "该 url 的订阅源已存在")
		return
	}

	// 先真抓一次再入库：坏源根本进不了库，不用等下一轮抓取才暴露。
	// 显式创建为停用状态的源跳过校验，保留“先登记、稍后修”的用法。
	if feed.Enabled {
		if err := validateFeedSource(feed.URL); err != nil {
			writeError(w, http.StatusUnprocessableEntity, err.Error())
			return
		}
	}

	if err := db.Create(&feed).Error; err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusCreated, feed)
}

func UpdateFeed(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseUint(r.PathValue("id"), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "非法的 feed id")
		return
	}
	req, err := decodeFeedRequest(r)
	if err != nil {
		writeError(w, http.StatusBadRequest, "请求体格式错误: "+err.Error())
		return
	}

	db := database.GetDB()
	var feed models.Feed
	if err := db.First(&feed, id).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			writeError(w, http.StatusNotFound, "订阅源不存在")
			return
		}
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	updates := map[string]any{}
	if req.Name != nil {
		name := strings.TrimSpace(*req.Name)
		if name == "" {
			writeError(w, http.StatusBadRequest, "name 不能为空")
			return
		}
		updates["name"] = name
	}
	if req.URL != nil {
		raw := strings.TrimSpace(*req.URL)
		if err := validateFeedURL(raw); err != nil {
			writeError(w, http.StatusBadRequest, err.Error())
			return
		}
		var clash models.Feed
		if err := db.Where("url = ? AND id <> ?", raw, id).First(&clash).Error; err == nil {
			writeError(w, http.StatusConflict, "该 url 的订阅源已存在")
			return
		}
		updates["url"] = raw
	}
	if req.Enabled != nil {
		updates["enabled"] = *req.Enabled
	}
	if len(updates) == 0 {
		writeError(w, http.StatusBadRequest, "没有需要更新的字段")
		return
	}

	if err := db.Model(&feed).Updates(updates).Error; err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if err := db.First(&feed, id).Error; err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, feed)
}

// UploadFeedCover 接收 multipart 表单中的 image 字段，归一化为目标尺寸后
// 保存为订阅源自定义封面。条目自身没有图片时优先使用这张图。
func UploadFeedCover(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseUint(r.PathValue("id"), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "非法的 feed id")
		return
	}

	db := database.GetDB()
	var feed models.Feed
	if err := db.First(&feed, id).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			writeError(w, http.StatusNotFound, "订阅源不存在")
			return
		}
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	r.Body = http.MaxBytesReader(w, r.Body, maxCoverUpload)
	if err := r.ParseMultipartForm(maxCoverUpload); err != nil {
		writeError(w, http.StatusBadRequest, "封面上传失败：文件过大或表单格式错误")
		return
	}
	file, _, err := r.FormFile("image")
	if err != nil {
		writeError(w, http.StatusBadRequest, "缺少 image 文件字段")
		return
	}
	defer file.Close()

	data, err := io.ReadAll(io.LimitReader(file, maxCoverUpload+1))
	if err != nil {
		writeError(w, http.StatusBadRequest, "读取封面失败: "+err.Error())
		return
	}
	if len(data) == 0 {
		writeError(w, http.StatusBadRequest, "封面文件为空")
		return
	}
	if len(data) > maxCoverUpload {
		writeError(w, http.StatusRequestEntityTooLarge, "封面文件不能超过 8 MB")
		return
	}

	rel, err := cover.SaveUpload(ImageDir(), data)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	if err := db.Model(&feed).Update("cover_image_path", rel).Error; err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if err := db.First(&feed, id).Error; err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, feed)
}

// DeleteFeedCover 清除自定义封面，回到按订阅源名称生成的默认封面。
func DeleteFeedCover(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseUint(r.PathValue("id"), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "非法的 feed id")
		return
	}

	db := database.GetDB()
	var feed models.Feed
	if err := db.First(&feed, id).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			writeError(w, http.StatusNotFound, "订阅源不存在")
			return
		}
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if err := db.Model(&feed).Update("cover_image_path", "").Error; err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	if err := db.First(&feed, id).Error; err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, feed)
}

// DeleteFeed 删除订阅源及其条目；条目的评分与曝光记录一并清理，
// 避免 next 取到无主条目后返回 500。
func DeleteFeed(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseUint(r.PathValue("id"), 10, 64)
	if err != nil {
		writeError(w, http.StatusBadRequest, "非法的 feed id")
		return
	}

	db := database.GetDB()
	var feed models.Feed
	if err := db.First(&feed, id).Error; err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			writeError(w, http.StatusNotFound, "订阅源不存在")
			return
		}
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}

	err = db.Transaction(func(tx *gorm.DB) error {
		var ids []uint
		if err := tx.Model(&models.Item{}).Where("feed_id = ?", id).Pluck("id", &ids).Error; err != nil {
			return err
		}
		if len(ids) > 0 {
			if err := tx.Where("item_id IN ?", ids).Delete(&models.DeviceItemView{}).Error; err != nil {
				return err
			}
			if err := tx.Where("item_id IN ?", ids).Delete(&models.ItemRating{}).Error; err != nil {
				return err
			}
			if err := tx.Where("item_id IN ?", ids).Delete(&models.ItemNfcOpen{}).Error; err != nil {
				return err
			}
			if err := tx.Where("id IN ?", ids).Delete(&models.Item{}).Error; err != nil {
				return err
			}
		}
		return tx.Delete(&models.Feed{}, id).Error
	})
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// scanImages 统计图片目录的文件数与占用（jpg 为准）。
func scanImages(dir string) ImageStats {
	var stats ImageStats
	filepath.WalkDir(dir, func(path string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		if !strings.HasSuffix(strings.ToLower(d.Name()), ".jpg") {
			return nil
		}
		if info, err := d.Info(); err == nil {
			stats.Files++
			stats.Bytes += info.Size()
		}
		return nil
	})
	return stats
}

// imageDir 由 main 在启动时注入，供 /admin/status 统计占用。
var imageDir string

func SetImageDir(dir string) {
	imageDir = dir
}

func ImageDir() string {
	return imageDir
}
