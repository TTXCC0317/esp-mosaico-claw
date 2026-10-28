/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_settings.h"
#include "qrcodegen.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_gsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mosaic_app_catalog.h"
#include "mosaic_app_shell.h"
#include "mosaic_i18n.h"
#include "mosaic_top_notice.h"
#include "mosaic_hub_actions.h"
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "mosaic_loader.h"
#endif
#include "mosaic_runtime.h"
#include "settings_actions.h"
#include "settings_binds.h"
#include "settings_objects.h"
#include "settings_root_icon_data.h"
#include "settings_templates.h"

#define SETTINGS_TOGGLE_OFF_COLOR UINT32_C(0x39E7)
#define SETTINGS_TOGGLE_ON_COLOR  UINT32_C(0xFA60)
#define SETTINGS_TEXT_COLOR       UINT32_C(0xFFFF)
#define SETTINGS_MUTED_COLOR      UINT32_C(0x9493)

/* Runtime English/Chinese selector for strings owned by the C layer. */
#define TR(en, zh) mosaic_i18n_tr((en), (zh))
#define SETTINGS_LEVEL_OFF_COLOR  UINT32_C(0x18C3)
#define SETTINGS_LEVEL_ON_COLOR   UINT32_C(0xFA60)
#define SETTINGS_DEFAULT_VOLUME 80
#define SETTINGS_DEFAULT_BRIGHTNESS 100
#define SETTINGS_INTEGRATIONS_MODEL_PAGE 9
#define SETTINGS_INTEGRATIONS_CHANNELS_PAGE 10
#define SETTINGS_DISPLAY_PAGE 1
#define SETTINGS_SOUND_PAGE 11
#define SETTINGS_ABOUT_PAGE 2
#define SETTINGS_SECURITY_PAGE 12
#define SETTINGS_DEBUG_PAGE 13
#define SETTINGS_DETAIL_PAGE 7
#define SETTINGS_FACTORY_PAGE 8
#define SETTINGS_FACTORY_HOLD_US INT64_C(3000000)
#define SETTINGS_FACTORY_HOLD_X_MIN 64
#define SETTINGS_FACTORY_HOLD_X_MAX 416
#define SETTINGS_FACTORY_HOLD_Y_MIN 300
#define SETTINGS_FACTORY_HOLD_Y_MAX 386
#define SETTINGS_FACTORY_HOLD_WIDTH 352
#define SETTINGS_TRACK_X_MIN 12
#define SETTINGS_TRACK_X_MAX 468
#define SETTINGS_LEVEL_COUNT 24
#define SETTINGS_DISPLAY_VIEWPORT_Y 64
#define SETTINGS_DISPLAY_VIEWPORT_H 400
#define SETTINGS_BRIGHTNESS_TRACK_Y_MIN 176
#define SETTINGS_BRIGHTNESS_TRACK_Y_MAX 238
#define SETTINGS_VOLUME_TRACK_Y_MIN 176
#define SETTINGS_VOLUME_TRACK_Y_MAX 238
#define SETTINGS_NOTIFICATION_Y_MIN 279
#define SETTINGS_NOTIFICATION_Y_MAX 327
#define SETTINGS_DISPLAY_TAP_SLOP 16
#define SETTINGS_DISPLAY_RENDER_PERIOD_MS 16
#define SETTINGS_QR_SIZE 104U
#define SETTINGS_CHANNEL_QR_SIZE 104U
#define SETTINGS_QR_STRIDE (SETTINGS_QR_SIZE * 2U)
#define SETTINGS_QR_BYTES ((size_t)SETTINGS_QR_STRIDE * SETTINGS_QR_SIZE)
#define SETTINGS_WLAN_PAGE 3
#define SETTINGS_WLAN_DETAIL_PAGE 4
#define SETTINGS_WLAN_PASSWORD_PAGE 5
#define SETTINGS_WLAN_PHONE_PAGE 6
#define SETTINGS_WLAN_PHONE_QR_SIZE 184U
#define SETTINGS_WLAN_PASSWORD_MIN 8
#define SETTINGS_NOTICE_DURATION_MS 3200U
#define SETTINGS_WLAN_LOADING_FRAME_COUNT 8U
#define SETTINGS_WLAN_LOADING_PERIOD_MS 125U
#define SETTINGS_WLAN_SCAN_REFRESH_US INT64_C(15000000)
#define SETTINGS_WLAN_SCAN_RETRY_US INT64_C(3000000)
#define SETTINGS_WLAN_SCAN_TIMEOUT_US INT64_C(12000000)
#define SETTINGS_WLAN_CONNECT_TIMEOUT_US INT64_C(35000000)
#define SETTINGS_INTEGRATION_PROGRESS_COUNT 12U
#define SETTINGS_INTEGRATION_PROGRESS_OFF UINT32_C(0x39E7)
#define SETTINGS_INTEGRATION_PROGRESS_ON UINT32_C(0xEA04)
#define SETTINGS_INTEGRATION_CONFIG_US 2200000LL
#define SETTINGS_INTEGRATION_QR_QUIET 2
/* Three retained Canvas frames plus one replacement frame. */
#define SETTINGS_INTEGRATION_QR_FRAMES 4U
#define SETTINGS_INTEGRATION_QR_SIZE 256U
#define SETTINGS_INTEGRATION_QR_STRIDE (SETTINGS_INTEGRATION_QR_SIZE * 2U)
#define SETTINGS_INTEGRATION_QR_BYTES \
    ((size_t)SETTINGS_INTEGRATION_QR_STRIDE * SETTINGS_INTEGRATION_QR_SIZE)

typedef enum {
    SETTINGS_LLM_STATUS = 0,
    SETTINGS_LLM_CONFIGURING,
    SETTINGS_LLM_PROGRESS,
} settings_llm_phase_t;

typedef struct {
    uint8_t *pixels;
    atomic_bool busy;
} settings_qr_frame_t;

static const mosaic_top_notice_config_t s_top_notice = {
    .visible_bind = GSP_BIND_SETTINGS_TOP_NOTICE_VISIBLE,
    .title_bind = GSP_BIND_SETTINGS_TOP_NOTICE_TITLE,
    .message_bind = GSP_BIND_SETTINGS_TOP_NOTICE_MESSAGE,
};

typedef struct {
    bool enabled;
    bool connected;
    bool auto_join;
    mosaic_settings_wifi_state_t state;
    char current_ssid[33];
    char selected_ssid[33];
    char password[65];
} settings_wlan_state_t;

typedef enum {
    SETTINGS_DISPLAY_TAP_NONE = 0,
    SETTINGS_DISPLAY_TAP_NOTIFICATION,
} settings_display_tap_t;

typedef struct {
    mosaic_settings_ops_t ops;
    mosaic_settings_snapshot_t snapshot;
    esp_gsp_handle_t ui;
    int32_t brightness;
    int32_t volume;
    bool brightness_hw_available;
    bool sound_effect_enabled;
    bool factory_hold_active;
    bool factory_hold_completed;
    int64_t factory_hold_started_us;
    bool display_page_active;
    bool display_reset_pending;
    bool display_pointer_pressed;
    bool display_pointer_scrolled;
    int32_t display_press_y;
    int32_t display_last_y;
    settings_display_tap_t display_tap;
    bool brightness_drag_active;
    bool brightness_dirty;
    bool volume_drag_active;
    bool volume_dirty;
    settings_llm_phase_t llm_phase;
    uint16_t integration_page;
    int64_t integration_started_us;
    int64_t integration_deadline_us;
    uint8_t integration_rendered_progress;
    uint8_t *qr_temp;
    uint8_t *qr_code;
    settings_qr_frame_t qr_frames[SETTINGS_INTEGRATION_QR_FRAMES];
    char rendered_channel_qr[MOSAIC_SETTINGS_LLM_URL_LEN];
    char rendered_llm_qr[MOSAIC_SETTINGS_LLM_URL_LEN];
    char rendered_phone_qr[MOSAIC_SETTINGS_PHONE_QR_LEN];
    char provisioning_ap_ssid[MOSAIC_SETTINGS_SSID_LEN];
    atomic_bool display_render_pending;
    void *display_render_timer;
    bool wlan_page_active;
    bool wlan_entry_pending;
    bool wlan_leave_pending;
    bool wlan_detail_active;
    bool wlan_detail_leave_pending;
    bool wlan_password_active;
    bool wlan_phone_active;
    bool wlan_phone_exit_pending;
    uint32_t wlan_phone_operation_id;
    bool wlan_forget_pending;
    bool wlan_connect_pending;
    uint32_t wlan_connect_operation_id;
    int64_t wlan_connect_deadline_us;
    uint32_t wlan_scan_revision;
    int64_t wlan_scan_next_us;
    bool wlan_scan_waiting;
    void *wlan_loading_timer;
    uint8_t wlan_loading_frame;
    bool wlan_loading_visible;
    bool wlan_notice_pending;
    const char *wlan_notice_title;
    const char *wlan_notice_message;
    bool wlan_scan_compact;
    bool wlan_scan_compact_valid;
    bool wlan_list_attached;
    int64_t wlan_toggle_last_us;
    settings_wlan_state_t wlan;
} settings_ui_state_t;

static mosaic_settings_wifi_ap_t
    s_wlan_networks[MOSAIC_SETTINGS_WIFI_SCAN_MAX];
static size_t s_wlan_network_count;

typedef struct {
    esp_gsp_list_t handle;
    size_t total;
    bool dirty;
} settings_wlan_list_t;

static settings_wlan_list_t s_wlan_lists[2];
static esp_gsp_list_t s_root_list = ESP_GSP_LIST_NONE;
static esp_gsp_list_t s_detail_list = ESP_GSP_LIST_NONE;

typedef enum {
    SETTINGS_DETAIL_ABOUT,
    SETTINGS_DETAIL_SAFE_MODE,
    SETTINGS_DETAIL_BATTERY,
    SETTINGS_DETAIL_LANGUAGE,
    SETTINGS_DETAIL_CITY,
} settings_detail_kind_t;

static settings_detail_kind_t s_detail_kind = SETTINGS_DETAIL_ABOUT;
static const char *TAG = "settings_app";
static void settings_wlan_request_scan(void);
static void settings_wlan_phone_refresh(esp_gsp_handle_t ui);
static void settings_wlan_complete_phone_setup(esp_gsp_handle_t ui);
static void settings_wlan_apply_phone_status(
    esp_gsp_handle_t ui, const mosaic_settings_network_t *network);
static settings_ui_state_t s_state = {
    .brightness = SETTINGS_DEFAULT_BRIGHTNESS,
    .volume = SETTINGS_DEFAULT_VOLUME,
    .brightness_hw_available = true,
    .sound_effect_enabled = true,
    .snapshot.weather_city_index = -1,
    .wlan = {
        .enabled = true,
        .auto_join = true,
    },
};

typedef struct {
    mosaic_battery_event_cb_t callback;
    void *user_ctx;
} mosaic_battery_subscriber_t;

typedef struct {
    mosaic_wifi_event_cb_t callback;
    void *user_ctx;
} mosaic_wifi_subscriber_t;

#if defined(ESP_PLATFORM)
static portMUX_TYPE s_battery_lock = portMUX_INITIALIZER_UNLOCKED;
#define MOSAIC_BATTERY_LOCK()   portENTER_CRITICAL(&s_battery_lock)
#define MOSAIC_BATTERY_UNLOCK() portEXIT_CRITICAL(&s_battery_lock)
static portMUX_TYPE s_wifi_lock = portMUX_INITIALIZER_UNLOCKED;
#define MOSAIC_WIFI_LOCK()   portENTER_CRITICAL(&s_wifi_lock)
#define MOSAIC_WIFI_UNLOCK() portEXIT_CRITICAL(&s_wifi_lock)
#else
#define MOSAIC_BATTERY_LOCK()   ((void)0)
#define MOSAIC_BATTERY_UNLOCK() ((void)0)
#define MOSAIC_WIFI_LOCK()   ((void)0)
#define MOSAIC_WIFI_UNLOCK() ((void)0)
#endif
static mosaic_settings_battery_t s_battery_cache;
static bool s_battery_cache_valid;
static mosaic_battery_subscriber_t
    s_battery_subscribers[MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX];
static mosaic_settings_network_t s_wifi_cache;
static bool s_wifi_cache_valid;
static mosaic_wifi_subscriber_t
    s_wifi_subscribers[MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX];

static void keep_first_error(esp_err_t *result, esp_err_t candidate)
{
    if (*result == ESP_OK && candidate != ESP_OK) {
        *result = candidate;
    }
}

esp_err_t mosaic_settings_configure(const mosaic_settings_ops_t *ops)
{
    if (ops == NULL || ops->get_snapshot == NULL ||
            ops->set_rotation == NULL || ops->set_brightness == NULL ||
            ops->set_volume == NULL ||
            ops->request_network_reconfigure == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_state.ops = *ops;
    return ESP_OK;
}

esp_err_t mosaic_settings_set_brightness(int brightness, bool persist)
{
    if (brightness < 0 || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state.ops.set_brightness == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.set_brightness(
        s_state.ops.user_ctx, brightness, persist);
}

esp_err_t mosaic_settings_set_volume(int volume, bool persist)
{
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state.ops.set_volume == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.set_volume(s_state.ops.user_ctx, volume, persist);
}

esp_err_t mosaic_settings_set_vibration(bool enabled)
{
    if (s_state.ops.set_vibration == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.set_vibration(s_state.ops.user_ctx, enabled);
}

esp_err_t mosaic_settings_set_screen_timeout(uint32_t timeout_ms)
{
    if (s_state.ops.set_screen_timeout == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.set_screen_timeout(s_state.ops.user_ctx, timeout_ms);
}

esp_err_t mosaic_settings_set_language(const char *tag)
{
    if (tag == NULL || tag[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state.ops.set_language == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.set_language(s_state.ops.user_ctx, tag);
}

/* Built-in city presets for the Location page, grouped by province/region.
 * Coordinates are the city centres used for the Open-Meteo forecast lookup. */
static const mosaic_settings_city_t s_prov_beijing[] = {
    {"Beijing", "北京", 39.9042, 116.4074},
};
static const mosaic_settings_city_t s_prov_tianjin[] = {
    {"Tianjin", "天津", 39.3434, 117.3616},
};
static const mosaic_settings_city_t s_prov_shanghai[] = {
    {"Shanghai", "上海", 31.2304, 121.4737},
};
static const mosaic_settings_city_t s_prov_chongqing[] = {
    {"Chongqing", "重庆", 29.5630, 106.5516},
};
static const mosaic_settings_city_t s_prov_hebei[] = {
    {"Shijiazhuang", "石家庄", 38.0428, 114.5149},
    {"Tangshan", "唐山", 39.6304, 118.1802},
    {"Qinhuangdao", "秦皇岛", 39.9354, 119.5998},
    {"Handan", "邯郸", 36.6256, 114.5391},
    {"Xingtai", "邢台", 37.0708, 114.5048},
    {"Baoding", "保定", 38.8740, 115.4646},
    {"Zhangjiakou", "张家口", 40.7686, 114.8869},
    {"Chengde", "承德", 40.9515, 117.9634},
    {"Cangzhou", "沧州", 38.3045, 116.8387},
    {"Langfang", "廊坊", 39.5379, 116.6835},
    {"Hengshui", "衡水", 37.7388, 115.6706},
};
static const mosaic_settings_city_t s_prov_shanxi[] = {
    {"Taiyuan", "太原", 37.8706, 112.5489},
    {"Datong", "大同", 40.0903, 113.2958},
    {"Yangquan", "阳泉", 37.8570, 113.5805},
    {"Changzhi", "长治", 36.1954, 113.1163},
    {"Jincheng", "晋城", 35.4907, 112.8513},
    {"Shuozhou", "朔州", 39.3317, 112.4329},
    {"Jinzhong", "晋中", 37.6872, 112.7525},
    {"Yuncheng", "运城", 35.0263, 111.0039},
    {"Xinzhou", "忻州", 38.4167, 112.7337},
    {"Linfen", "临汾", 36.0880, 111.5190},
    {"Lvliang", "吕梁", 37.5187, 111.1344},
};
static const mosaic_settings_city_t s_prov_inner_mongolia[] = {
    {"Hohhot", "呼和浩特", 40.8424, 111.7490},
    {"Baotou", "包头", 40.6574, 109.8403},
    {"Wuhai", "乌海", 39.6533, 106.8228},
    {"Chifeng", "赤峰", 42.2586, 118.8878},
    {"Tongliao", "通辽", 43.6524, 122.2436},
    {"Ordos", "鄂尔多斯", 39.6086, 109.7810},
    {"Hulunbuir", "呼伦贝尔", 49.2116, 119.7658},
    {"Bayannur", "巴彦淖尔", 40.7432, 107.3870},
    {"Ulanqab", "乌兰察布", 40.9944, 113.1328},
};
static const mosaic_settings_city_t s_prov_liaoning[] = {
    {"Shenyang", "沈阳", 41.8057, 123.4315},
    {"Dalian", "大连", 38.9140, 121.6147},
    {"Anshan", "鞍山", 41.1086, 122.9946},
    {"Fushun", "抚顺", 41.8797, 123.9573},
    {"Benxi", "本溪", 41.2942, 123.7666},
    {"Dandong", "丹东", 40.1281, 124.3546},
    {"Jinzhou", "锦州", 41.0955, 121.1270},
    {"Yingkou", "营口", 40.6674, 122.2354},
    {"Fuxin", "阜新", 42.0216, 121.6701},
    {"Liaoyang", "辽阳", 41.2697, 123.2360},
    {"Panjin", "盘锦", 41.1244, 122.0708},
    {"Tieling", "铁岭", 42.2863, 123.8430},
    {"Chaoyang", "朝阳", 41.5768, 120.4509},
    {"Huludao", "葫芦岛", 40.7110, 120.8370},
};
static const mosaic_settings_city_t s_prov_jilin[] = {
    {"Changchun", "长春", 43.8171, 125.3235},
    {"Jilin", "吉林", 43.8378, 126.5494},
    {"Siping", "四平", 43.1664, 124.3504},
    {"Liaoyuan", "辽源", 42.8877, 125.1437},
    {"Tonghua", "通化", 41.7283, 125.9399},
    {"Baishan", "白山", 41.9400, 126.4239},
    {"Songyuan", "松原", 45.1411, 124.8252},
    {"Baicheng", "白城", 45.6196, 122.8410},
    {"Yanji", "延吉", 42.9048, 129.5092},
};
static const mosaic_settings_city_t s_prov_heilongjiang[] = {
    {"Harbin", "哈尔滨", 45.8038, 126.5349},
    {"Qiqihar", "齐齐哈尔", 47.3543, 123.9180},
    {"Jixi", "鸡西", 45.2955, 130.9690},
    {"Hegang", "鹤岗", 47.3500, 130.2976},
    {"Shuangyashan", "双鸭山", 46.6436, 131.1576},
    {"Daqing", "大庆", 46.5891, 125.1039},
    {"Yichun", "伊春", 47.7280, 128.8417},
    {"Jiamusi", "佳木斯", 46.7998, 130.3189},
    {"Qitaihe", "七台河", 45.7711, 131.0032},
    {"Mudanjiang", "牡丹江", 44.5527, 129.6329},
    {"Heihe", "黑河", 50.2451, 127.5285},
    {"Suihua", "绥化", 46.6373, 126.9906},
};
static const mosaic_settings_city_t s_prov_jiangsu[] = {
    {"Nanjing", "南京", 32.0603, 118.7969},
    {"Wuxi", "无锡", 31.4912, 120.3119},
    {"Xuzhou", "徐州", 34.2044, 117.2857},
    {"Changzhou", "常州", 31.8107, 119.9740},
    {"Suzhou", "苏州", 31.2989, 120.5853},
    {"Nantong", "南通", 31.9802, 120.8943},
    {"Lianyungang", "连云港", 34.5967, 119.2216},
    {"Huaian", "淮安", 33.5515, 119.0212},
    {"Yancheng", "盐城", 33.3496, 120.1615},
    {"Yangzhou", "扬州", 32.3942, 119.4129},
    {"Zhenjiang", "镇江", 32.1879, 119.4250},
    {"Taizhou", "泰州", 32.4849, 119.9153},
    {"Suqian", "宿迁", 33.9630, 118.2754},
};
static const mosaic_settings_city_t s_prov_zhejiang[] = {
    {"Hangzhou", "杭州", 30.2741, 120.1551},
    {"Ningbo", "宁波", 29.8683, 121.5440},
    {"Wenzhou", "温州", 27.9949, 120.6994},
    {"Jiaxing", "嘉兴", 30.7522, 120.7500},
    {"Huzhou", "湖州", 30.8927, 120.0863},
    {"Shaoxing", "绍兴", 30.0303, 120.5802},
    {"Jinhua", "金华", 29.0785, 119.6474},
    {"Quzhou", "衢州", 28.9359, 118.8747},
    {"Zhoushan", "舟山", 29.9853, 122.2072},
    {"Taizhou", "台州", 28.6560, 121.4207},
    {"Lishui", "丽水", 28.4674, 119.9229},
};
static const mosaic_settings_city_t s_prov_anhui[] = {
    {"Hefei", "合肥", 31.8206, 117.2272},
    {"Wuhu", "芜湖", 31.3529, 118.4330},
    {"Bengbu", "蚌埠", 32.9166, 117.3893},
    {"Huainan", "淮南", 32.6255, 116.9998},
    {"Maanshan", "马鞍山", 31.6705, 118.5063},
    {"Huaibei", "淮北", 33.9558, 116.7983},
    {"Tongling", "铜陵", 30.9445, 117.8121},
    {"Anqing", "安庆", 30.5433, 117.0631},
    {"Huangshan", "黄山", 29.7147, 118.3376},
    {"Chuzhou", "滁州", 32.3018, 118.3168},
    {"Fuyang", "阜阳", 32.8969, 115.8143},
    {"Suzhou", "宿州", 33.6461, 116.9642},
    {"Luan", "六安", 31.7337, 116.5219},
    {"Bozhou", "亳州", 33.8446, 115.7787},
    {"Chizhou", "池州", 30.6600, 117.4914},
    {"Xuancheng", "宣城", 30.9406, 118.7584},
};
static const mosaic_settings_city_t s_prov_fujian[] = {
    {"Fuzhou", "福州", 26.0745, 119.2965},
    {"Xiamen", "厦门", 24.4798, 118.0894},
    {"Putian", "莆田", 25.4310, 119.0079},
    {"Sanming", "三明", 26.2634, 117.6390},
    {"Quanzhou", "泉州", 24.8741, 118.6757},
    {"Zhangzhou", "漳州", 24.5130, 117.6472},
    {"Nanping", "南平", 26.6436, 118.1778},
    {"Longyan", "龙岩", 25.0751, 117.0179},
    {"Ningde", "宁德", 26.6657, 119.5480},
};
static const mosaic_settings_city_t s_prov_jiangxi[] = {
    {"Nanchang", "南昌", 28.6820, 115.8579},
    {"Jingdezhen", "景德镇", 29.2687, 117.1784},
    {"Pingxiang", "萍乡", 27.6229, 113.8549},
    {"Jiujiang", "九江", 29.7051, 116.0019},
    {"Xinyu", "新余", 27.8174, 114.9171},
    {"Yingtan", "鹰潭", 28.2600, 117.0690},
    {"Ganzhou", "赣州", 25.8310, 114.9350},
    {"Jian", "吉安", 27.1116, 114.9928},
    {"Yichun", "宜春", 27.8141, 114.4162},
    {"Fuzhou", "抚州", 27.9492, 116.3581},
    {"Shangrao", "上饶", 28.4548, 117.9433},
};
static const mosaic_settings_city_t s_prov_shandong[] = {
    {"Jinan", "济南", 36.6512, 117.1201},
    {"Qingdao", "青岛", 36.0671, 120.3826},
    {"Zibo", "淄博", 36.8135, 118.0550},
    {"Zaozhuang", "枣庄", 34.8107, 117.3238},
    {"Dongying", "东营", 37.4340, 118.6748},
    {"Yantai", "烟台", 37.4638, 121.4479},
    {"Weifang", "潍坊", 36.7069, 119.1618},
    {"Jining", "济宁", 35.4152, 116.5870},
    {"Taian", "泰安", 36.1949, 117.1285},
    {"Weihai", "威海", 37.5128, 122.1201},
    {"Rizhao", "日照", 35.4164, 119.5268},
    {"Linyi", "临沂", 35.1045, 118.3565},
    {"Dezhou", "德州", 37.4355, 116.3574},
    {"Liaocheng", "聊城", 36.4569, 115.9852},
    {"Binzhou", "滨州", 37.3827, 117.9728},
    {"Heze", "菏泽", 35.2333, 115.4800},
};
static const mosaic_settings_city_t s_prov_henan[] = {
    {"Zhengzhou", "郑州", 34.7466, 113.6254},
    {"Kaifeng", "开封", 34.7970, 114.3076},
    {"Luoyang", "洛阳", 34.6197, 112.4540},
    {"Pingdingshan", "平顶山", 33.7662, 113.1926},
    {"Anyang", "安阳", 36.0977, 114.3931},
    {"Hebi", "鹤壁", 35.7470, 114.2972},
    {"Xinxiang", "新乡", 35.3029, 113.9268},
    {"Jiaozuo", "焦作", 35.2159, 113.2418},
    {"Puyang", "濮阳", 35.7616, 115.0292},
    {"Xuchang", "许昌", 34.0357, 113.8526},
    {"Luohe", "漯河", 33.5759, 114.0166},
    {"Sanmenxia", "三门峡", 34.7733, 111.2005},
    {"Nanyang", "南阳", 32.9908, 112.5285},
    {"Shangqiu", "商丘", 34.4146, 115.6560},
    {"Xinyang", "信阳", 32.1470, 114.0913},
    {"Zhoukou", "周口", 33.6204, 114.6969},
    {"Zhumadian", "驻马店", 32.9831, 114.0228},
};
static const mosaic_settings_city_t s_prov_hubei[] = {
    {"Wuhan", "武汉", 30.5928, 114.3055},
    {"Huangshi", "黄石", 30.2000, 115.0389},
    {"Shiyan", "十堰", 32.6294, 110.7989},
    {"Yichang", "宜昌", 30.6919, 111.2865},
    {"Xiangyang", "襄阳", 32.0090, 112.1226},
    {"Ezhou", "鄂州", 30.3919, 114.8948},
    {"Jingmen", "荆门", 31.0353, 112.1993},
    {"Xiaogan", "孝感", 30.9245, 113.9163},
    {"Jingzhou", "荆州", 30.3350, 112.2410},
    {"Huanggang", "黄冈", 30.4537, 114.8722},
    {"Xianning", "咸宁", 29.8412, 114.3224},
    {"Suizhou", "随州", 31.6902, 113.3826},
    {"Enshi", "恩施", 30.2721, 109.4883},
};
static const mosaic_settings_city_t s_prov_hunan[] = {
    {"Changsha", "长沙", 28.2282, 112.9388},
    {"Zhuzhou", "株洲", 27.8279, 113.1340},
    {"Xiangtan", "湘潭", 27.8296, 112.9446},
    {"Hengyang", "衡阳", 26.8933, 112.5718},
    {"Shaoyang", "邵阳", 27.2389, 111.4677},
    {"Yueyang", "岳阳", 29.3574, 113.1289},
    {"Changde", "常德", 29.0316, 111.6984},
    {"Zhangjiajie", "张家界", 29.1170, 110.4792},
    {"Yiyang", "益阳", 28.5539, 112.3552},
    {"Chenzhou", "郴州", 25.7707, 113.0146},
    {"Yongzhou", "永州", 26.4204, 111.6134},
    {"Huaihua", "怀化", 27.5501, 109.9785},
    {"Loudi", "娄底", 27.7001, 111.9960},
    {"Xiangxi", "湘西", 28.3146, 109.7400},
};
static const mosaic_settings_city_t s_prov_guangdong[] = {
    {"Guangzhou", "广州", 23.1291, 113.2644},
    {"Shenzhen", "深圳", 22.5431, 114.0579},
    {"Zhuhai", "珠海", 22.2710, 113.5767},
    {"Shantou", "汕头", 23.3535, 116.6822},
    {"Foshan", "佛山", 23.0218, 113.1219},
    {"Shaoguan", "韶关", 24.8107, 113.5972},
    {"Zhanjiang", "湛江", 21.2707, 110.3594},
    {"Zhaoqing", "肇庆", 23.0470, 112.4654},
    {"Jiangmen", "江门", 22.5789, 113.0819},
    {"Maoming", "茂名", 21.6632, 110.9255},
    {"Huizhou", "惠州", 23.1115, 114.4160},
    {"Meizhou", "梅州", 24.2991, 116.1224},
    {"Shanwei", "汕尾", 22.7864, 115.3753},
    {"Heyuan", "河源", 23.7437, 114.7000},
    {"Yangjiang", "阳江", 21.8580, 111.9823},
    {"Qingyuan", "清远", 23.6817, 113.0560},
    {"Dongguan", "东莞", 23.0207, 113.7518},
    {"Zhongshan", "中山", 22.5170, 113.3928},
    {"Chaozhou", "潮州", 23.6567, 116.6226},
    {"Jieyang", "揭阳", 23.5497, 116.3726},
    {"Yunfu", "云浮", 22.9151, 112.0443},
};
static const mosaic_settings_city_t s_prov_guangxi[] = {
    {"Nanning", "南宁", 22.8170, 108.3665},
    {"Liuzhou", "柳州", 24.3146, 109.4280},
    {"Guilin", "桂林", 25.2736, 110.2900},
    {"Wuzhou", "梧州", 23.4767, 111.2794},
    {"Beihai", "北海", 21.4813, 109.1199},
    {"Fangchenggang", "防城港", 21.6868, 108.3540},
    {"Qinzhou", "钦州", 21.9675, 108.6542},
    {"Guigang", "贵港", 23.1115, 109.5987},
    {"Yulin", "玉林", 22.6540, 110.1647},
    {"Baise", "百色", 23.9022, 106.6186},
    {"Hezhou", "贺州", 24.4038, 111.5671},
    {"Hechi", "河池", 24.6959, 108.0851},
    {"Laibin", "来宾", 23.7521, 109.2215},
    {"Chongzuo", "崇左", 22.3760, 107.3646},
};
static const mosaic_settings_city_t s_prov_hainan[] = {
    {"Haikou", "海口", 20.0440, 110.1988},
    {"Sanya", "三亚", 18.2528, 109.5119},
    {"Sansha", "三沙", 16.8328, 112.3481},
    {"Danzhou", "儋州", 19.5209, 109.5807},
};
static const mosaic_settings_city_t s_prov_sichuan[] = {
    {"Chengdu", "成都", 30.5728, 104.0668},
    {"Zigong", "自贡", 29.3390, 104.7785},
    {"Panzhihua", "攀枝花", 26.5821, 101.7188},
    {"Luzhou", "泸州", 28.8717, 105.4433},
    {"Deyang", "德阳", 31.1270, 104.3979},
    {"Mianyang", "绵阳", 31.4675, 104.6796},
    {"Guangyuan", "广元", 32.4358, 105.8431},
    {"Suining", "遂宁", 30.5328, 105.5926},
    {"Neijiang", "内江", 29.5802, 105.0584},
    {"Leshan", "乐山", 29.5521, 103.7660},
    {"Nanchong", "南充", 30.8373, 106.1106},
    {"Meishan", "眉山", 30.0754, 103.8485},
    {"Yibin", "宜宾", 28.7513, 104.6433},
    {"Guangan", "广安", 30.4562, 106.6334},
    {"Dazhou", "达州", 31.2090, 107.4680},
    {"Yaan", "雅安", 30.0106, 103.0010},
    {"Bazhong", "巴中", 31.8584, 106.7532},
    {"Ziyang", "资阳", 30.1222, 104.6279},
};
static const mosaic_settings_city_t s_prov_guizhou[] = {
    {"Guiyang", "贵阳", 26.6470, 106.6302},
    {"Liupanshui", "六盘水", 26.5925, 104.8301},
    {"Zunyi", "遵义", 27.7256, 106.9270},
    {"Anshun", "安顺", 26.2456, 105.9476},
    {"Bijie", "毕节", 27.2840, 105.2930},
    {"Tongren", "铜仁", 27.7186, 109.1910},
};
static const mosaic_settings_city_t s_prov_yunnan[] = {
    {"Kunming", "昆明", 25.0389, 102.7183},
    {"Qujing", "曲靖", 25.4901, 103.7960},
    {"Yuxi", "玉溪", 24.3516, 102.5430},
    {"Baoshan", "保山", 25.1120, 99.1610},
    {"Zhaotong", "昭通", 27.3380, 103.7170},
    {"Lijiang", "丽江", 26.8720, 100.2300},
    {"Puer", "普洱", 22.8250, 100.9660},
    {"Lincang", "临沧", 23.8870, 100.0890},
};
static const mosaic_settings_city_t s_prov_tibet[] = {
    {"Lhasa", "拉萨", 29.6520, 91.1721},
    {"Shigatse", "日喀则", 29.2695, 88.8800},
    {"Qamdo", "昌都", 31.1360, 97.1720},
    {"Nyingchi", "林芝", 29.6500, 94.3620},
    {"Shannan", "山南", 29.2380, 91.7660},
    {"Nagqu", "那曲", 31.4762, 92.0524},
};
static const mosaic_settings_city_t s_prov_shaanxi[] = {
    {"Xian", "西安", 34.3416, 108.9398},
    {"Tongchuan", "铜川", 34.8967, 108.9453},
    {"Baoji", "宝鸡", 34.3610, 107.2374},
    {"Xianyang", "咸阳", 34.3299, 108.7092},
    {"Weinan", "渭南", 34.4994, 109.5097},
    {"Yanan", "延安", 36.5853, 109.4900},
    {"Hanzhong", "汉中", 33.0776, 107.0233},
    {"Yulin", "榆林", 38.2856, 109.7346},
    {"Ankang", "安康", 32.6903, 109.0292},
    {"Shangluo", "商洛", 33.8683, 109.9405},
};
static const mosaic_settings_city_t s_prov_gansu[] = {
    {"Lanzhou", "兰州", 36.0611, 103.8343},
    {"Jiayuguan", "嘉峪关", 39.7719, 98.2891},
    {"Jinchang", "金昌", 38.5200, 102.1880},
    {"Baiyin", "白银", 36.5448, 104.1386},
    {"Tianshui", "天水", 34.5809, 105.7245},
    {"Wuwei", "武威", 37.9330, 102.6380},
    {"Zhangye", "张掖", 38.9257, 100.4498},
    {"Jiuquan", "酒泉", 39.7440, 98.5080},
    {"Qingyang", "庆阳", 35.7099, 107.6426},
    {"Pingliang", "平凉", 35.5417, 106.6840},
    {"Dingxi", "定西", 35.5790, 104.6260},
    {"Longnan", "陇南", 33.4000, 104.9290},
};
static const mosaic_settings_city_t s_prov_qinghai[] = {
    {"Xining", "西宁", 36.6171, 101.7782},
    {"Haidong", "海东", 36.5020, 102.1040},
};
static const mosaic_settings_city_t s_prov_ningxia[] = {
    {"Yinchuan", "银川", 38.4872, 106.2309},
    {"Shizuishan", "石嘴山", 38.9840, 106.3760},
    {"Wuzhong", "吴忠", 37.9860, 106.1980},
    {"Guyuan", "固原", 36.0040, 106.2850},
    {"Zhongwei", "中卫", 37.5140, 105.1900},
};
static const mosaic_settings_city_t s_prov_xinjiang[] = {
    {"Urumqi", "乌鲁木齐", 43.8256, 87.6168},
    {"Karamay", "克拉玛依", 45.5790, 84.8890},
    {"Turpan", "吐鲁番", 42.9476, 89.1840},
    {"Hami", "哈密", 42.8330, 93.5150},
};
static const mosaic_settings_city_t s_prov_taiwan[] = {
    {"Taipei", "台北", 25.0330, 121.5654},
    {"New Taipei", "新北", 25.0169, 121.4628},
    {"Taichung", "台中", 24.1477, 120.6736},
    {"Tainan", "台南", 22.9997, 120.2270},
    {"Kaohsiung", "高雄", 22.6273, 120.3014},
    {"Hsinchu", "新竹", 24.8138, 120.9675},
    {"Keelung", "基隆", 25.1276, 121.7392},
    {"Chiayi", "嘉义", 23.4801, 120.4491},
};
static const mosaic_settings_city_t s_prov_hong_kong[] = {
    {"Hong Kong", "香港", 22.3193, 114.1694},
};
static const mosaic_settings_city_t s_prov_macau[] = {
    {"Macau", "澳门", 22.1987, 113.5439},
};
static const mosaic_settings_city_t s_prov_international[] = {
    {"Hong Kong", "香港", 22.3193, 114.1694},
    {"Tokyo", "东京", 35.6762, 139.6503},
    {"New York", "纽约", 40.7128, -74.0060},
    {"London", "伦敦", 51.5074, -0.1278},
    {"Paris", "巴黎", 48.8566, 2.3522},
    {"Sydney", "悉尼", -33.8688, 151.2093},
};

static const mosaic_settings_province_t s_weather_provinces[] = {
    {"Beijing", "北京", s_prov_beijing, sizeof(s_prov_beijing) / sizeof(s_prov_beijing[0])},
    {"Tianjin", "天津", s_prov_tianjin, sizeof(s_prov_tianjin) / sizeof(s_prov_tianjin[0])},
    {"Shanghai", "上海", s_prov_shanghai, sizeof(s_prov_shanghai) / sizeof(s_prov_shanghai[0])},
    {"Chongqing", "重庆", s_prov_chongqing, sizeof(s_prov_chongqing) / sizeof(s_prov_chongqing[0])},
    {"Hebei", "河北", s_prov_hebei, sizeof(s_prov_hebei) / sizeof(s_prov_hebei[0])},
    {"Shanxi", "山西", s_prov_shanxi, sizeof(s_prov_shanxi) / sizeof(s_prov_shanxi[0])},
    {"Inner Mongolia", "内蒙古", s_prov_inner_mongolia, sizeof(s_prov_inner_mongolia) / sizeof(s_prov_inner_mongolia[0])},
    {"Liaoning", "辽宁", s_prov_liaoning, sizeof(s_prov_liaoning) / sizeof(s_prov_liaoning[0])},
    {"Jilin", "吉林", s_prov_jilin, sizeof(s_prov_jilin) / sizeof(s_prov_jilin[0])},
    {"Heilongjiang", "黑龙江", s_prov_heilongjiang, sizeof(s_prov_heilongjiang) / sizeof(s_prov_heilongjiang[0])},
    {"Jiangsu", "江苏", s_prov_jiangsu, sizeof(s_prov_jiangsu) / sizeof(s_prov_jiangsu[0])},
    {"Zhejiang", "浙江", s_prov_zhejiang, sizeof(s_prov_zhejiang) / sizeof(s_prov_zhejiang[0])},
    {"Anhui", "安徽", s_prov_anhui, sizeof(s_prov_anhui) / sizeof(s_prov_anhui[0])},
    {"Fujian", "福建", s_prov_fujian, sizeof(s_prov_fujian) / sizeof(s_prov_fujian[0])},
    {"Jiangxi", "江西", s_prov_jiangxi, sizeof(s_prov_jiangxi) / sizeof(s_prov_jiangxi[0])},
    {"Shandong", "山东", s_prov_shandong, sizeof(s_prov_shandong) / sizeof(s_prov_shandong[0])},
    {"Henan", "河南", s_prov_henan, sizeof(s_prov_henan) / sizeof(s_prov_henan[0])},
    {"Hubei", "湖北", s_prov_hubei, sizeof(s_prov_hubei) / sizeof(s_prov_hubei[0])},
    {"Hunan", "湖南", s_prov_hunan, sizeof(s_prov_hunan) / sizeof(s_prov_hunan[0])},
    {"Guangdong", "广东", s_prov_guangdong, sizeof(s_prov_guangdong) / sizeof(s_prov_guangdong[0])},
    {"Guangxi", "广西", s_prov_guangxi, sizeof(s_prov_guangxi) / sizeof(s_prov_guangxi[0])},
    {"Hainan", "海南", s_prov_hainan, sizeof(s_prov_hainan) / sizeof(s_prov_hainan[0])},
    {"Sichuan", "四川", s_prov_sichuan, sizeof(s_prov_sichuan) / sizeof(s_prov_sichuan[0])},
    {"Guizhou", "贵州", s_prov_guizhou, sizeof(s_prov_guizhou) / sizeof(s_prov_guizhou[0])},
    {"Yunnan", "云南", s_prov_yunnan, sizeof(s_prov_yunnan) / sizeof(s_prov_yunnan[0])},
    {"Tibet", "西藏", s_prov_tibet, sizeof(s_prov_tibet) / sizeof(s_prov_tibet[0])},
    {"Shaanxi", "陕西", s_prov_shaanxi, sizeof(s_prov_shaanxi) / sizeof(s_prov_shaanxi[0])},
    {"Gansu", "甘肃", s_prov_gansu, sizeof(s_prov_gansu) / sizeof(s_prov_gansu[0])},
    {"Qinghai", "青海", s_prov_qinghai, sizeof(s_prov_qinghai) / sizeof(s_prov_qinghai[0])},
    {"Ningxia", "宁夏", s_prov_ningxia, sizeof(s_prov_ningxia) / sizeof(s_prov_ningxia[0])},
    {"Xinjiang", "新疆", s_prov_xinjiang, sizeof(s_prov_xinjiang) / sizeof(s_prov_xinjiang[0])},
    {"Taiwan", "台湾", s_prov_taiwan, sizeof(s_prov_taiwan) / sizeof(s_prov_taiwan[0])},
    {"Hong Kong", "香港", s_prov_hong_kong, sizeof(s_prov_hong_kong) / sizeof(s_prov_hong_kong[0])},
    {"Macau", "澳门", s_prov_macau, sizeof(s_prov_macau) / sizeof(s_prov_macau[0])},
    {"International", "国际", s_prov_international, sizeof(s_prov_international) / sizeof(s_prov_international[0])},
};

size_t mosaic_settings_province_count(void)
{
    return sizeof(s_weather_provinces) / sizeof(s_weather_provinces[0]);
}

const mosaic_settings_province_t *mosaic_settings_province(size_t index)
{
    if (index >= mosaic_settings_province_count()) {
        return NULL;
    }
    return &s_weather_provinces[index];
}

size_t mosaic_settings_city_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < mosaic_settings_province_count(); ++i) {
        count += s_weather_provinces[i].city_count;
    }
    return count;
}

const mosaic_settings_city_t *mosaic_settings_city(size_t index)
{
    for (size_t i = 0; i < mosaic_settings_province_count(); ++i) {
        const mosaic_settings_province_t *province = &s_weather_provinces[i];
        if (index < province->city_count) {
            return &province->cities[index];
        }
        index -= province->city_count;
    }
    return NULL;
}

esp_err_t mosaic_settings_set_weather_city(int city_index)
{
    if (s_state.ops.set_weather_city == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.set_weather_city(s_state.ops.user_ctx, city_index);
}

esp_err_t mosaic_settings_factory_reset(void)
{
    if (s_state.ops.factory_reset == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.factory_reset(s_state.ops.user_ctx);
}

esp_err_t mosaic_settings_get_snapshot(
    mosaic_settings_snapshot_t *ret_snapshot)
{
    if (ret_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ret_snapshot, 0, sizeof(*ret_snapshot));
    if (s_state.ops.get_snapshot == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.get_snapshot(s_state.ops.user_ctx, ret_snapshot);
}

esp_err_t mosaic_settings_set_wifi_enabled(bool enabled)
{
    if (s_state.ops.set_wifi_enabled == NULL) {
        return ESP_OK;
    }
    return s_state.ops.set_wifi_enabled(s_state.ops.user_ctx, enabled);
}

bool mosaic_settings_wifi_backend_available(void)
{
    return s_state.ops.get_wifi_status != NULL &&
        s_state.ops.scan_wifi != NULL &&
        s_state.ops.connect_wifi != NULL;
}

esp_err_t mosaic_settings_request_wifi_scan(void)
{
    if (s_state.ops.request_wifi_scan == NULL) {
        /* Synchronous/fake providers expose their results through scan_wifi
         * directly and need no separate request operation. */
        return s_state.ops.scan_wifi != NULL ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.request_wifi_scan(s_state.ops.user_ctx);
}

esp_err_t mosaic_settings_scan_wifi(mosaic_settings_wifi_ap_t *records,
                                    size_t capacity, size_t *out_count)
{
    if (records == NULL || capacity == 0 || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state.ops.scan_wifi == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.scan_wifi(
        s_state.ops.user_ctx, records, capacity, out_count);
}

esp_err_t mosaic_settings_connect_wifi(
    const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state.ops.connect_wifi == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.connect_wifi(s_state.ops.user_ctx, ssid, password);
}

esp_err_t mosaic_settings_forget_wifi(void)
{
    if (s_state.ops.forget_wifi == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.forget_wifi(s_state.ops.user_ctx);
}

esp_err_t mosaic_settings_get_phone_setup(
    char *ap_ssid, size_t ap_ssid_size,
    char *qr_payload, size_t qr_payload_size)
{
    if (ap_ssid == NULL || ap_ssid_size == 0U ||
            qr_payload == NULL || qr_payload_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state.ops.get_phone_setup == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_state.ops.get_phone_setup(
        s_state.ops.user_ctx, ap_ssid, ap_ssid_size,
        qr_payload, qr_payload_size);
}

esp_err_t mosaic_settings_get_battery(mosaic_settings_battery_t *ret_battery)
{
    if (ret_battery == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ret_battery, 0, sizeof(*ret_battery));

    MOSAIC_BATTERY_LOCK();
    const bool cached = s_battery_cache_valid;
    mosaic_settings_battery_t local = s_battery_cache;
    MOSAIC_BATTERY_UNLOCK();
    if (cached) {
        *ret_battery = local;
        return ESP_OK;
    }

    if (s_state.ops.get_battery != NULL) {
        esp_err_t err =
            s_state.ops.get_battery(s_state.ops.user_ctx, ret_battery);
        if (err == ESP_OK) {
            mosaic_settings_notify_battery(ret_battery);
        }
        return err;
    }
    if (s_state.ops.get_snapshot == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    mosaic_settings_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = s_state.ops.get_snapshot(s_state.ops.user_ctx, snapshot);
    if (err == ESP_OK) {
        *ret_battery = snapshot->battery;
        mosaic_settings_notify_battery(ret_battery);
    }
    free(snapshot);
    return err;
}

void mosaic_settings_notify_battery(const mosaic_settings_battery_t *info)
{
    if (info == NULL) {
        return;
    }
    mosaic_settings_battery_t published = *info;
    mosaic_battery_subscriber_t listeners[MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX];
    size_t listener_count = 0;

    MOSAIC_BATTERY_LOCK();
    if (!s_battery_cache_valid) {
        published.sequence = 1U;
    } else if (published.sequence == 0U ||
               published.sequence <= s_battery_cache.sequence) {
        published.sequence = s_battery_cache.sequence + 1U;
    }
    s_battery_cache = published;
    s_battery_cache_valid = true;
    for (size_t i = 0; i < MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX; ++i) {
        if (s_battery_subscribers[i].callback != NULL) {
            listeners[listener_count++] = s_battery_subscribers[i];
        }
    }
    MOSAIC_BATTERY_UNLOCK();

    for (size_t i = 0; i < listener_count; ++i) {
        listeners[i].callback(&published, listeners[i].user_ctx);
    }
}

esp_err_t mosaic_settings_subscribe_battery(mosaic_battery_event_cb_t cb,
                                            void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool delivered = false;
    mosaic_settings_battery_t snapshot = {0};

    MOSAIC_BATTERY_LOCK();
    for (size_t i = 0; i < MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX; ++i) {
        if (s_battery_subscribers[i].callback == cb &&
                s_battery_subscribers[i].user_ctx == user_ctx) {
            if (s_battery_cache_valid) {
                snapshot = s_battery_cache;
                delivered = true;
            }
            MOSAIC_BATTERY_UNLOCK();
            if (delivered) {
                cb(&snapshot, user_ctx);
            }
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX; ++i) {
        if (s_battery_subscribers[i].callback == NULL) {
            s_battery_subscribers[i].callback = cb;
            s_battery_subscribers[i].user_ctx = user_ctx;
            if (s_battery_cache_valid) {
                snapshot = s_battery_cache;
                delivered = true;
            }
            MOSAIC_BATTERY_UNLOCK();
            if (delivered) {
                cb(&snapshot, user_ctx);
            }
            return ESP_OK;
        }
    }
    MOSAIC_BATTERY_UNLOCK();
    return ESP_ERR_NO_MEM;
}

esp_err_t mosaic_settings_unsubscribe_battery(mosaic_battery_event_cb_t cb,
                                              void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    MOSAIC_BATTERY_LOCK();
    for (size_t i = 0; i < MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX; ++i) {
        if (s_battery_subscribers[i].callback == cb &&
                s_battery_subscribers[i].user_ctx == user_ctx) {
            s_battery_subscribers[i].callback = NULL;
            s_battery_subscribers[i].user_ctx = NULL;
            MOSAIC_BATTERY_UNLOCK();
            return ESP_OK;
        }
    }
    MOSAIC_BATTERY_UNLOCK();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t mosaic_settings_get_wifi(mosaic_settings_network_t *ret_network)
{
    if (ret_network == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ret_network, 0, sizeof(*ret_network));

    /* Prefer the live provider so Hub can refresh RSSI without waiting for
     * a Wi-Fi state event. Cache is updated quietly (no subscriber fan-out). */
    if (s_state.ops.get_wifi_status != NULL) {
        esp_err_t err =
            s_state.ops.get_wifi_status(s_state.ops.user_ctx, ret_network);
        if (err == ESP_OK) {
            MOSAIC_WIFI_LOCK();
            s_wifi_cache = *ret_network;
            s_wifi_cache_valid = true;
            MOSAIC_WIFI_UNLOCK();
        }
        return err;
    }

    MOSAIC_WIFI_LOCK();
    const bool cached = s_wifi_cache_valid;
    mosaic_settings_network_t local = s_wifi_cache;
    MOSAIC_WIFI_UNLOCK();
    if (cached) {
        *ret_network = local;
        return ESP_OK;
    }
    if (s_state.ops.get_snapshot == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    mosaic_settings_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = s_state.ops.get_snapshot(s_state.ops.user_ctx, snapshot);
    if (err == ESP_OK) {
        *ret_network = snapshot->network;
        mosaic_settings_notify_wifi(ret_network);
    }
    free(snapshot);
    return err;
}

void mosaic_settings_notify_wifi(const mosaic_settings_network_t *info)
{
    if (info == NULL) {
        return;
    }
    mosaic_settings_network_t published = *info;
    mosaic_wifi_subscriber_t listeners[MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX];
    size_t listener_count = 0;

    MOSAIC_WIFI_LOCK();
    s_wifi_cache = published;
    s_wifi_cache_valid = true;
    for (size_t i = 0; i < MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX; ++i) {
        if (s_wifi_subscribers[i].callback != NULL) {
            listeners[listener_count++] = s_wifi_subscribers[i];
        }
    }
    MOSAIC_WIFI_UNLOCK();

    for (size_t i = 0; i < listener_count; ++i) {
        listeners[i].callback(&published, listeners[i].user_ctx);
    }
}

esp_err_t mosaic_settings_subscribe_wifi(mosaic_wifi_event_cb_t cb,
                                         void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool delivered = false;
    mosaic_settings_network_t snapshot = {0};

    MOSAIC_WIFI_LOCK();
    for (size_t i = 0; i < MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX; ++i) {
        if (s_wifi_subscribers[i].callback == cb &&
                s_wifi_subscribers[i].user_ctx == user_ctx) {
            if (s_wifi_cache_valid) {
                snapshot = s_wifi_cache;
                delivered = true;
            }
            MOSAIC_WIFI_UNLOCK();
            if (delivered) {
                cb(&snapshot, user_ctx);
            }
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX; ++i) {
        if (s_wifi_subscribers[i].callback == NULL) {
            s_wifi_subscribers[i].callback = cb;
            s_wifi_subscribers[i].user_ctx = user_ctx;
            if (s_wifi_cache_valid) {
                snapshot = s_wifi_cache;
                delivered = true;
            }
            MOSAIC_WIFI_UNLOCK();
            if (delivered) {
                cb(&snapshot, user_ctx);
            }
            return ESP_OK;
        }
    }
    MOSAIC_WIFI_UNLOCK();
    return ESP_ERR_NO_MEM;
}

esp_err_t mosaic_settings_unsubscribe_wifi(mosaic_wifi_event_cb_t cb,
                                           void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    MOSAIC_WIFI_LOCK();
    for (size_t i = 0; i < MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX; ++i) {
        if (s_wifi_subscribers[i].callback == cb &&
                s_wifi_subscribers[i].user_ctx == user_ctx) {
            s_wifi_subscribers[i].callback = NULL;
            s_wifi_subscribers[i].user_ctx = NULL;
            MOSAIC_WIFI_UNLOCK();
            return ESP_OK;
        }
    }
    MOSAIC_WIFI_UNLOCK();
    return ESP_ERR_NOT_FOUND;
}

/* Screen-off choices shown in the Display picker drawer, ordered to match the
 * timeout_options_ms table in the SCREEN_TIMEOUT_SELECT handler. */
static const char *const s_timeout_names_en[] = {
    "10 sec", "30 sec", "1 min", "2 min", "5 min", "Never",
};
static const char *const s_timeout_names_zh[] = {
    "10 秒", "30 秒", "1 分钟", "2 分钟", "5 分钟", "从不",
};

static void settings_apply_screen_timeout_texts(esp_gsp_handle_t ui,
                                                uint32_t selected)
{
    const char *const *names = mosaic_i18n_is_chinese()
        ? s_timeout_names_zh : s_timeout_names_en;
    if (selected >= 6U) {
        selected = 1U;
    }
    (void)gsp_settings_settings_screen_timeout_option_0_set_text(ui, names[0]);
    (void)gsp_settings_settings_screen_timeout_option_1_set_text(ui, names[1]);
    (void)gsp_settings_settings_screen_timeout_option_2_set_text(ui, names[2]);
    (void)gsp_settings_settings_screen_timeout_option_3_set_text(ui, names[3]);
    (void)gsp_settings_settings_screen_timeout_option_4_set_text(ui, names[4]);
    (void)gsp_settings_settings_screen_timeout_option_5_set_text(ui, names[5]);
    (void)gsp_settings_settings_screen_timeout_value_set_text(ui,
                                                              names[selected]);
}

static esp_err_t settings_display_render(esp_gsp_handle_t ui)
{
    char value[8];
    const uint32_t timeout_ms = s_state.snapshot.screen_timeout_ms;
    uint32_t timeout_index = 5U;
    if (timeout_ms == 10000U) {
        timeout_index = 0U;
    } else if (timeout_ms == 30000U) {
        timeout_index = 1U;
    } else if (timeout_ms == 60000U) {
        timeout_index = 2U;
    } else if (timeout_ms == 120000U) {
        timeout_index = 3U;
    } else if (timeout_ms == 300000U) {
        timeout_index = 4U;
    }
    uint32_t rotation_index = s_state.snapshot.rotation / 90U;
    if (rotation_index > 3U) {
        rotation_index = 0U;
    }
    esp_gsp_err_t err = gsp_settings_settings_rotation_dropdown_set_selected(ui, rotation_index);
    (void)snprintf(value, sizeof(value), "%" PRId32 "%%",
                   s_state.brightness);
    if (err == ESP_GSP_OK) {
        err =
        gsp_settings_settings_brightness_value_set_text(ui, value);
    }
    /* The screen-off picker is a Drawer of labels, so both the six choices and
     * the summary value are re-texted here in the active language. */
    settings_apply_screen_timeout_texts(ui, timeout_index);
    if (err == ESP_GSP_OK) {
        (void)snprintf(value, sizeof(value), "%" PRId32 "%%",
                       s_state.volume);
        err = gsp_settings_settings_volume_value_set_text(ui, value);
    }
    int32_t brightness_width =
        (s_state.brightness * (SETTINGS_TRACK_X_MAX - SETTINGS_TRACK_X_MIN) +
         50) / 100;
    int32_t volume_width =
        (s_state.volume * (SETTINGS_TRACK_X_MAX - SETTINGS_TRACK_X_MIN) +
         50) / 100;
    if (brightness_width < 1) {
        brightness_width = 1;
    }
    if (volume_width < 1) {
        volume_width = 1;
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_brightness_fill_set_w(
            ui, brightness_width);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_volume_fill_set_w(ui, volume_width);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_notification_sound_set_checked(
            ui, s_state.sound_effect_enabled);
    }
    return err == ESP_GSP_OK ? ESP_OK : ESP_FAIL;
}

static void settings_display_request_render(void)
{
    atomic_store_explicit(
        &s_state.display_render_pending, true, memory_order_release);
}

static void settings_display_render_timer_cb(
    esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    if (ui != s_state.ui) {
        return;
    }
    if (s_state.factory_hold_active) {
        uint16_t top = 0;
        if (esp_gsp_stack_view_get_top(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, &top) != ESP_GSP_OK ||
                top != SETTINGS_FACTORY_PAGE) {
            s_state.factory_hold_active = false;
            (void)gsp_settings_settings_factory_hold_progress_set_value(
                ui, 0);
        } else {
            const int64_t elapsed =
                esp_timer_get_time() - s_state.factory_hold_started_us;
            int32_t progress = (int32_t)(elapsed * 100 /
                                         SETTINGS_FACTORY_HOLD_US);
            if (progress < 0) progress = 0;
            if (progress > 100) {
                progress = 100;
            }
            (void)gsp_settings_settings_factory_hold_progress_set_value(
                ui, progress);
            if (elapsed >= SETTINGS_FACTORY_HOLD_US) {
                s_state.factory_hold_active = false;
                s_state.factory_hold_completed = true;
                esp_err_t err = mosaic_settings_factory_reset();
                if (err == ESP_OK) {
                    (void)mosaic_top_notice_show(
                        ui, &s_top_notice,
                        TR("Erase Started", "开始清除"),
                        TR("Restarting to finish reset", "重启以完成恢复"),
                        SETTINGS_NOTICE_DURATION_MS);
                } else {
                    s_state.factory_hold_completed = false;
                    (void)gsp_settings_settings_factory_hold_progress_set_value(
                        ui, 0);
                    (void)mosaic_top_notice_show(
                        ui, &s_top_notice,
                        TR("Reset Failed", "恢复失败"),
                        TR("Please try again", "请重试"),
                        SETTINGS_NOTICE_DURATION_MS);
                }
            }
        }
    }
    if (atomic_exchange_explicit(
            &s_state.display_render_pending, false,
            memory_order_acq_rel)) {
        esp_err_t err = settings_display_render(ui);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "deferred Display render failed: %s",
                     esp_err_to_name(err));
        }
    }
}

typedef struct {
    const char *title;
    const char *title_zh;
    const char *value;
    bool ok;
    bool danger;
    bool action;
} settings_detail_row_t;

static const settings_detail_row_t s_about_rows[] = {
    {"Model", "型号", "ESP-MOSAICO", false, false, false},
    {"Software", "软件版本", "--", false, false, false},
    {"GSP", "GSP", "--", false, false, false},
    {"Display", "显示", "2.16\" 480×480", false, false, false},
    {"Serial", "序列号", "--", false, false, false},
};

static const settings_detail_row_t s_safe_rows[] = {
    {"Third-Party Skills", "第三方技能", "Disabled", false, false, false},
    {"Network", "网络", "Wi-Fi Only", false, false, false},
    {"Accessories", "配件", "Read Only", false, false, false},
    {"Exit Safe Mode", "退出安全模式", "Restart", false, true, true},
};

static const settings_detail_row_t s_battery_rows[] = {
    {"Charge", "充电", "--", false, false, false},
    {"Voltage", "电压", "--", false, false, false},
    {"Current", "电流", "--", false, false, false},
};

/* Language names are shown natively; the value column marks the active one. */
static const settings_detail_row_t s_language_rows[] = {
    {"English", "English", "", false, false, false},
    {"中文",    "中文",    "", false, false, false},
};

/* Location rows are built in two levels: a province list ("Auto (IP)" plus
 * one row per province) and, once a province is entered, its city list. */
#define SETTINGS_CITY_ROW_MAX 48U
static settings_detail_row_t s_city_rows[SETTINGS_CITY_ROW_MAX];
static size_t s_city_row_count;
/* -1 shows the province list; >= 0 is the entered province's city list. */
static int s_city_province = -1;

static size_t settings_province_city_base(size_t province_index)
{
    size_t base = 0;
    for (size_t i = 0; i < province_index; ++i) {
        const mosaic_settings_province_t *province =
            mosaic_settings_province(i);
        if (province != NULL) {
            base += province->city_count;
        }
    }
    return base;
}

static void settings_city_rows_build(void)
{
    size_t index = 0;
    const int active = s_state.snapshot.weather_city_index;
    if (s_city_province >= 0) {
        const mosaic_settings_province_t *province =
            mosaic_settings_province((size_t)s_city_province);
        if (province == NULL) {
            const char *auto_name = TR("Auto (IP)", "自动 (IP)");
            s_city_rows[index++] =
                (settings_detail_row_t){auto_name, auto_name, "",
                                        false, false, false};
            s_city_row_count = index;
            return;
        }
        const size_t base = settings_province_city_base((size_t)s_city_province);
        for (size_t i = 0; i < province->city_count &&
                index < SETTINGS_CITY_ROW_MAX; ++i) {
            const bool selected = ((int)(base + i) == active);
            const char *city_name = TR(province->cities[i].name,
                                       province->cities[i].name_zh);
            s_city_rows[index] = (settings_detail_row_t){
                city_name, city_name,
                selected ? "✓" : "",
                selected, false, false};
            ++index;
        }
        s_city_row_count = index;
        return;
    }
    const bool auto_selected = active < 0;
    {
        const char *auto_name = TR("Auto (IP)", "自动 (IP)");
        s_city_rows[index++] = (settings_detail_row_t){
            auto_name, auto_name, auto_selected ? "✓" : "",
            auto_selected, false, false};
    }
    const size_t province_count = mosaic_settings_province_count();
    for (size_t p = 0; p < province_count && index < SETTINGS_CITY_ROW_MAX; ++p) {
        const mosaic_settings_province_t *province =
            mosaic_settings_province(p);
        if (province == NULL) {
            continue;
        }
        const size_t base = settings_province_city_base(p);
        const char *value = "";
        bool ok = false;
        if (active >= 0 && (size_t)active >= base &&
                (size_t)active < base + province->city_count) {
            const mosaic_settings_city_t *selected_city =
                &province->cities[(size_t)active - base];
            value = TR(selected_city->name, selected_city->name_zh);
            ok = true;
        }
        const char *province_name = TR(province->name, province->name_zh);
        s_city_rows[index] = (settings_detail_row_t){
            province_name, province_name, value, ok, false, true};
        ++index;
    }
    s_city_row_count = index;
}

/* Values that need a Chinese form; anything unlisted (version strings, units,
 * "--", or text already localized before use) is returned unchanged. */
static const char *settings_detail_value_zh(const char *value)
{
    if (value == NULL || !mosaic_i18n_is_chinese()) {
        return value;
    }
    static const struct { const char *en; const char *zh; } k_values[] = {
        {"Disabled", "已禁用"},
        {"Wi-Fi Only", "仅 Wi-Fi"},
        {"Read Only", "只读"},
        {"Restart", "重启"},
    };
    for (size_t i = 0; i < sizeof(k_values) / sizeof(k_values[0]); ++i) {
        if (strcmp(value, k_values[i].en) == 0) {
            return k_values[i].zh;
        }
    }
    return value;
}

static const settings_detail_row_t *settings_detail_rows(size_t *count)
{
    switch (s_detail_kind) {
    case SETTINGS_DETAIL_SAFE_MODE:
        *count = sizeof(s_safe_rows) / sizeof(s_safe_rows[0]);
        return s_safe_rows;
    case SETTINGS_DETAIL_BATTERY:
        *count = sizeof(s_battery_rows) / sizeof(s_battery_rows[0]);
        return s_battery_rows;
    case SETTINGS_DETAIL_LANGUAGE:
        *count = sizeof(s_language_rows) / sizeof(s_language_rows[0]);
        return s_language_rows;
    case SETTINGS_DETAIL_CITY:
        if (s_city_row_count == 0U) {
            settings_city_rows_build();
        }
        *count = s_city_row_count;
        return s_city_rows;
    case SETTINGS_DETAIL_ABOUT:
    default:
        *count = sizeof(s_about_rows) / sizeof(s_about_rows[0]);
        return s_about_rows;
    }
}

static gsp_err_t settings_detail_bind_item(
    esp_gsp_handle_t ui, esp_gsp_row_t row, uint32_t item_index,
    void *user_ctx)
{
    (void)user_ctx;
    size_t count = 0;
    const settings_detail_row_t *rows = settings_detail_rows(&count);
    if (item_index >= count) {
        return GSP_ERR_INVALID_ARG;
    }
    const settings_detail_row_t *item = &rows[item_index];
    char value_text[64];
    const char *value = settings_detail_value_zh(item->value);
    if (s_detail_kind == SETTINGS_DETAIL_ABOUT) {
        switch (item_index) {
        case 1:
            value = s_state.snapshot.software_version[0] != '\0'
                ? s_state.snapshot.software_version : "--";
            break;
        case 2:
            (void)snprintf(value_text, sizeof(value_text), "v%u.%u.%u",
                           GSP_VERSION_MAJOR, GSP_VERSION_MINOR,
                           GSP_VERSION_PATCH);
            value = value_text;
            break;
        case 4:
            value = s_state.snapshot.serial_number[0] != '\0' ? s_state.snapshot.serial_number : "--";
            break;

        default:
            break;
        }
    } else if (s_detail_kind == SETTINGS_DETAIL_BATTERY &&
            s_state.snapshot.battery.available) {
        const mosaic_settings_battery_t *battery = &s_state.snapshot.battery;
        switch (item_index) {
        case 0:
            snprintf(value_text, sizeof(value_text), "%u%%",
                     battery->state_of_charge);
            value = value_text;
            break;
        case 1:
            snprintf(value_text, sizeof(value_text), "%u mV",
                     battery->voltage_mv);
            value = value_text;
            break;
        case 2:
            snprintf(value_text, sizeof(value_text), "%d mA",
                     (int)battery->current_ma);
            value = value_text;
            break;
        default:
            break;
        }
    } else if (s_detail_kind == SETTINGS_DETAIL_LANGUAGE) {
        const bool active = (item_index == 0U) != mosaic_i18n_is_chinese();
        value = active ? "✓" : "";
    }
    esp_gsp_err_t err = gsp_settings_settings_detail_row_row_set_title_text(
        ui, row, TR(item->title, item->title_zh));
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_detail_row_row_set_title_fg_color(
            ui, row, item->danger ? SETTINGS_TOGGLE_ON_COLOR
                                  : SETTINGS_TEXT_COLOR);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_detail_row_row_set_value_text(
            ui, row, value);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_detail_row_row_set_value_fg_color(
            ui, row, item->ok ? UINT32_C(0x6610) : SETTINGS_MUTED_COLOR);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_detail_row_row_set_chevron_text(
            ui, row, item->action ? "›" : "");
    }
    return err == ESP_GSP_OK ? GSP_OK : GSP_ERR_INVALID_STATE;
}

static void settings_detail_list_attach(esp_gsp_handle_t ui)
{
    if (s_detail_list == ESP_GSP_LIST_NONE) {
        s_detail_list = esp_gsp_list_bind_component(
            ui, GSP_OBJ_KEY_SETTINGS_DETAIL_LIST,
            settings_detail_bind_item, NULL);
    }
}

static void settings_open_detail(esp_gsp_handle_t ui,
                                 settings_detail_kind_t kind)
{
    static const char *titles[] = {"About", "Safe Mode", "Battery", "Language", "Location"};
    static const char *titles_zh[] = {"关于", "安全模式", "电池", "语言", "城市定位"};
    s_detail_kind = kind;
    if (kind == SETTINGS_DETAIL_CITY) {
        /* Always re-enter at the province list level. */
        s_city_province = -1;
        settings_city_rows_build();
    }
    size_t count = 0;
    (void)settings_detail_rows(&count);
    settings_detail_list_attach(ui);
    (void)gsp_settings_nav_settings_detail_title_set_text(
        ui, TR(titles[kind], titles_zh[kind]));
    (void)esp_gsp_list_set_total(ui, s_detail_list, count);
    (void)esp_gsp_list_refresh(ui, s_detail_list);
    (void)esp_gsp_list_scroll_to(ui, s_detail_list, 0);
    if (esp_gsp_stack_view_push(ui, GSP_OBJ_KEY_SETTINGS_STACK,
                               SETTINGS_DETAIL_PAGE, true) == ESP_GSP_OK) {
        mosaic_app_shell_set_root_visible(ui, false);
    }
}

/* Rebuild and refresh the Location detail list for the current level
 * (province list when s_city_province < 0, otherwise that province's cities),
 * updating the detail page title to match. */
static void settings_city_render_list(esp_gsp_handle_t ui)
{
    settings_city_rows_build();
    settings_detail_list_attach(ui);
    const char *title = TR("Location", "城市定位");
    if (s_city_province >= 0) {
        const mosaic_settings_province_t *province =
            mosaic_settings_province((size_t)s_city_province);
        if (province != NULL) {
            title = TR(province->name, province->name_zh);
        }
    }
    (void)gsp_settings_nav_settings_detail_title_set_text(ui, title);
    (void)esp_gsp_list_set_total(ui, s_detail_list, s_city_row_count);
    (void)esp_gsp_list_refresh(ui, s_detail_list);
}

typedef struct {
    const char *title;
    const char *title_zh;
    const uint8_t *icon;
    size_t icon_size;
} settings_root_row_t;

#define SETTINGS_ROOT_ROW(_title, _title_zh, _name) { \
    .title = (_title), \
    .title_zh = (_title_zh), \
    .icon = settings_root_icon_##_name, \
    .icon_size = sizeof(settings_root_icon_##_name), \
}

static const settings_root_row_t s_root_rows[] = {
    SETTINGS_ROOT_ROW("AI Model", "AI 模型", ai),
    SETTINGS_ROOT_ROW("IM Channels", "IM 通道", channels),
    SETTINGS_ROOT_ROW("Network", "网络", network),
    SETTINGS_ROOT_ROW("Display", "显示", display),
    SETTINGS_ROOT_ROW("Sound", "声音", sound),
    SETTINGS_ROOT_ROW("Security", "安全", security),
    SETTINGS_ROOT_ROW("Battery", "电池", battery),
    SETTINGS_ROOT_ROW("Location", "城市定位", location),
    SETTINGS_ROOT_ROW("Language", "语言", language),
    SETTINGS_ROOT_ROW("Debug", "调试", debug),
    SETTINGS_ROOT_ROW("About", "关于", about),
};

#define SETTINGS_ROOT_LOCATION_INDEX 7U
#define SETTINGS_ROOT_LANGUAGE_INDEX 8U
#define SETTINGS_ROOT_DEBUG_INDEX 9U
#define SETTINGS_ROOT_ABOUT_INDEX 10U

static const char *settings_wlan_summary(void)
{
    const settings_wlan_state_t *wlan = &s_state.wlan;
    if (!wlan->enabled || wlan->state == MOSAIC_SETTINGS_WIFI_DISABLED) {
        return TR("Off", "已关闭");
    }
    if (wlan->state == MOSAIC_SETTINGS_WIFI_SCANNING) {
        return TR("Scanning", "扫描中");
    }
    if (wlan->state == MOSAIC_SETTINGS_WIFI_CONNECTING) {
        return TR("Connecting", "连接中");
    }
    if (wlan->state == MOSAIC_SETTINGS_WIFI_AUTH_FAILED) {
        return TR("Wrong password", "密码错误");
    }
    if (wlan->state == MOSAIC_SETTINGS_WIFI_AP_NOT_FOUND) {
        return TR("Not found", "未找到");
    }
    if (wlan->state == MOSAIC_SETTINGS_WIFI_RETRY_WAIT) {
        return TR("Retrying", "重试中");
    }
    if (wlan->connected && wlan->current_ssid[0] != '\0') {
        return wlan->current_ssid;
    }
    return TR("Offline", "离线");
}

static bool settings_llm_is_bound(void)
{
    return mosaic_settings_llm_is_configured(&s_state.snapshot);
}

static bool settings_any_im_is_bound(void)
{
    return s_state.snapshot.im.wechat_configured ||
           s_state.snapshot.im.qq_configured ||
           s_state.snapshot.im.feishu_configured ||
           s_state.snapshot.im.telegram_configured;
}

static gsp_err_t settings_root_list_bind_item(
    esp_gsp_handle_t ui, esp_gsp_row_t row, uint32_t item_index,
    void *user_ctx)
{
    (void)user_ctx;
    if (item_index >= sizeof(s_root_rows) / sizeof(s_root_rows[0])) {
        return GSP_ERR_INVALID_ARG;
    }
    char value[16] = "";
    const char *value_text = value;
    switch (item_index) {
    case 0:
        value_text = settings_llm_is_bound() ? TR("Bound", "已绑定")
                                             : TR("Unbound", "未绑定");
        break;
    case 1:
        value_text = settings_any_im_is_bound() ? TR("Bound", "已绑定")
                                                : TR("Unbound", "未绑定");
        break;
    case 2:
        value_text = settings_wlan_summary();
        break;
    case 3:
        (void)snprintf(value, sizeof(value), "%" PRId32 "%%",
                       s_state.brightness);
        break;
    case 4:
        (void)snprintf(value, sizeof(value), "%" PRId32 "%%",
                       s_state.volume);
        break;
    case 6:
        if (s_state.snapshot.battery.available) {
            (void)snprintf(value, sizeof(value), "%u%%",
                           s_state.snapshot.battery.state_of_charge);
        } else {
            value_text = "--";
        }
        break;
    case SETTINGS_ROOT_LOCATION_INDEX: {
        const mosaic_settings_city_t *city =
            s_state.snapshot.weather_city_index >= 0
                ? mosaic_settings_city((size_t)s_state.snapshot.weather_city_index)
                : NULL;
        value_text = city != NULL ? TR(city->name, city->name_zh)
                                  : TR("Auto", "自动");
        break;
    }
    case SETTINGS_ROOT_LANGUAGE_INDEX:
        value_text = mosaic_i18n_is_chinese() ? "中文" : "English";
        break;
    default:
        break;
    }
    esp_gsp_err_t err =
        gsp_settings_settings_root_row_row_set_icon_resource(
            ui, row, s_root_rows[item_index].icon,
            s_root_rows[item_index].icon_size);
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_root_row_row_set_title_text(
            ui, row, TR(s_root_rows[item_index].title,
                        s_root_rows[item_index].title_zh));
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_root_row_row_set_value_text(
            ui, row, value_text);
    }
    return err;
}

static void settings_root_list_refresh(esp_gsp_handle_t ui)
{
    if (s_root_list != ESP_GSP_LIST_NONE) {
        (void)esp_gsp_list_refresh(ui, s_root_list);
    }
}

static void settings_root_list_attach(esp_gsp_handle_t ui)
{
    if (s_root_list == ESP_GSP_LIST_NONE) {
        s_root_list = esp_gsp_list_bind_component(
            ui, GSP_OBJ_KEY_SETTINGS_ROOT_LIST,
            settings_root_list_bind_item, NULL);
    }
    if (s_root_list == ESP_GSP_LIST_NONE) {
        ESP_LOGW(TAG, "bind Settings root list failed");
        return;
    }
    (void)esp_gsp_list_set_total(
        ui, s_root_list, sizeof(s_root_rows) / sizeof(s_root_rows[0]));
    settings_root_list_refresh(ui);
}

static gsp_err_t settings_wlan_list_bind_item(
    esp_gsp_handle_t ui, esp_gsp_row_t row, uint32_t item_index,
    void *user_ctx)
{
    (void)user_ctx;
    if (item_index >= s_wlan_network_count) {
        /* set_total(0) can retire a materialized row one update later. Keep
         * that transient row empty instead of turning normal List teardown
         * into a binding error. */
        return esp_gsp_row_text(ui, row, "");
    }
    return esp_gsp_row_text(ui, row, s_wlan_networks[item_index].ssid);
}

static bool settings_wlan_show_current(void)
{
    return s_state.wlan_page_active && s_state.wlan.enabled &&
           (s_state.wlan.connected || s_state.wlan_connect_pending) &&
           s_state.wlan.current_ssid[0] != '\0';
}

static void settings_wlan_lists_reset(void)
{
    for (size_t i = 0; i < 2; ++i) {
        s_wlan_lists[i] = (settings_wlan_list_t) {
            .handle = ESP_GSP_LIST_NONE,
            .dirty = true,
        };
    }
    s_state.wlan_list_attached = false;
}

static esp_gsp_err_t settings_wlan_apply_layout(esp_gsp_handle_t ui)
{
    const bool compact = !settings_wlan_show_current();
    if (s_state.wlan_scan_compact_valid &&
            s_state.wlan_scan_compact == compact) {
        return ESP_GSP_OK;
    }
    s_state.wlan_scan_compact_valid = false;
    esp_gsp_err_t err = compact
        ? gsp_settings_settings_wlan_scan_connected_layer_set_visible(ui, false)
        : gsp_settings_settings_wlan_scan_layer_set_visible(ui, false);
    if (err == ESP_GSP_OK) {
        err = compact
            ? gsp_settings_settings_wlan_scan_layer_set_visible(ui, true)
            : gsp_settings_settings_wlan_scan_connected_layer_set_visible(ui, true);
    }
    if (err == ESP_GSP_OK) {
        s_state.wlan_scan_compact = compact;
        s_state.wlan_scan_compact_valid = true;
    }
    return err;
}

static void settings_wlan_refresh_list(esp_gsp_handle_t ui)
{
    if (!s_state.wlan_list_attached) {
        return;
    }
    const size_t active = settings_wlan_show_current() ? 1 : 0;
    const gsp_component_key_t keys[] = {
        GSP_OBJ_KEY_SETTINGS_WLAN_SCAN_LIST,
        GSP_OBJ_KEY_SETTINGS_WLAN_SCAN_CONNECTED_LIST,
    };
    settings_wlan_list_t *inactive = &s_wlan_lists[1 - active];
    settings_wlan_list_t *list = &s_wlan_lists[active];
    esp_gsp_err_t err = settings_wlan_apply_layout(ui);
    if (err != ESP_GSP_OK) {
        goto failed;
    }
    if (inactive->handle != ESP_GSP_LIST_NONE && inactive->total != 0) {
        err = esp_gsp_list_set_total(ui, inactive->handle, 0);
        if (err != ESP_GSP_OK) {
            goto failed;
        }
        inactive->total = 0;
        inactive->dirty = true;
    }
    if (list->handle == ESP_GSP_LIST_NONE) {
        list->handle = esp_gsp_list_bind_component(
            ui, keys[active], settings_wlan_list_bind_item, NULL);
        if (list->handle == ESP_GSP_LIST_NONE) {
            ESP_LOGW(TAG, "bind WLAN scan list failed");
            return;
        }
    }
    if (list->total != s_wlan_network_count) {
        err = esp_gsp_list_set_total(ui, list->handle, s_wlan_network_count);
        if (err != ESP_GSP_OK) {
            goto failed;
        }
        list->total = s_wlan_network_count;
        list->dirty = true;
    }
    if (list->dirty) {
        err = esp_gsp_list_refresh(ui, list->handle);
        if (err != ESP_GSP_OK) {
            goto failed;
        }
        list->dirty = false;
    }
    return;

failed:
    ESP_LOGW(TAG, "refresh WLAN List failed: 0x%x", (unsigned)err);
}

static void settings_wlan_scan(esp_gsp_handle_t ui)
{
    mosaic_settings_wifi_ap_t
        networks[MOSAIC_SETTINGS_WIFI_SCAN_MAX] = {0};
    size_t count = 0;
    if (s_state.ops.scan_wifi == NULL) {
        return;
    }
    esp_err_t scan_err = s_state.ops.scan_wifi(
                             s_state.ops.user_ctx, networks,
                             MOSAIC_SETTINGS_WIFI_SCAN_MAX, &count);
    if (scan_err != ESP_OK) {
        if (!s_state.wlan_scan_waiting &&
                s_state.wlan_scan_next_us == 0) {
            s_state.wlan_scan_next_us = esp_timer_get_time();
        }
        return;
    }
    /* Host providers can publish a synchronous result without scan metadata.
     * Device providers complete through scan_revision below. */
    if (s_state.wlan_scan_waiting &&
            s_state.snapshot.network.scan_revision == 0U) {
        s_state.wlan_scan_waiting = false;
        s_state.wlan_scan_next_us = esp_timer_get_time() +
            SETTINGS_WLAN_SCAN_REFRESH_US;
    }
    if (count > MOSAIC_SETTINGS_WIFI_SCAN_MAX) {
        count = MOSAIC_SETTINGS_WIFI_SCAN_MAX;
    }
    bool changed = count != s_wlan_network_count;
    for (size_t i = 0; !changed && i < count; ++i) {
        changed = strcmp(s_wlan_networks[i].ssid, networks[i].ssid) != 0 ||
            s_wlan_networks[i].secured != networks[i].secured;
    }
    if (!changed) {
        return;
    }
    memcpy(s_wlan_networks, networks, count * sizeof(networks[0]));
    s_wlan_network_count = count;
    for (size_t i = 0; i < 2; ++i) {
        s_wlan_lists[i].dirty = true;
    }
    settings_wlan_refresh_list(ui);
}

static void settings_wlan_request_scan(void)
{
    if (s_state.ops.request_wifi_scan == NULL || !s_state.wlan.enabled ||
            s_state.snapshot.network.radio_state !=
                MOSAIC_SETTINGS_WIFI_RADIO_ON) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    esp_err_t err = s_state.ops.request_wifi_scan(s_state.ops.user_ctx);
    if (err == ESP_OK) {
        s_state.wlan_scan_waiting = true;
        s_state.wlan_scan_next_us = now + SETTINGS_WLAN_SCAN_TIMEOUT_US;
    } else {
        s_state.wlan_scan_waiting = false;
        s_state.wlan_scan_next_us = now + SETTINGS_WLAN_SCAN_RETRY_US;
        ESP_LOGW(TAG, "request WLAN scan failed: %s",
                 esp_err_to_name(err));
    }
}

static void settings_wlan_update_summary(esp_gsp_handle_t ui)
{
    const settings_wlan_state_t *wlan = &s_state.wlan;
    settings_root_list_refresh(ui);
    s_state.snapshot.network.connected =
        wlan->enabled && wlan->connected;
    if (wlan->connected && wlan->current_ssid[0] != '\0') {
        strlcpy(s_state.snapshot.network.ssid, wlan->current_ssid,
                sizeof(s_state.snapshot.network.ssid));
    }
}

static void settings_wlan_list_attach(esp_gsp_handle_t ui)
{
    s_state.wlan_list_attached = true;
    if (s_wlan_network_count == 0) {
        settings_wlan_scan(ui);
    }
    settings_wlan_refresh_list(ui);
}

static void settings_wlan_list_park(esp_gsp_handle_t ui)
{
    /* Dynamic row instances are owned by the List driver, not by the scene
     * layer that authored the viewport. Emptying the List is the explicit
     * lifecycle boundary that removes rows before the Settings root returns. */
    s_state.wlan_list_attached = false;
    for (size_t i = 0; i < 2; ++i) {
        settings_wlan_list_t *list = &s_wlan_lists[i];
        if (list->handle == ESP_GSP_LIST_NONE || list->total == 0) {
            continue;
        }
        esp_gsp_err_t err = esp_gsp_list_set_total(ui, list->handle, 0);
        if (err == ESP_GSP_OK) {
            list->total = 0;
            list->dirty = true;
        } else {
            ESP_LOGW(TAG, "park WLAN List failed: 0x%x", (unsigned)err);
        }
    }
}

static void settings_wlan_password_attach(esp_gsp_handle_t ui)
{
    s_state.wlan_password_active = true;
    mosaic_app_shell_set_bottom_enabled(ui, false);
    (void)esp_gsp_set_cursor(ui, GSP_BIND_SETTINGS_WLAN_PASSWORD_VALUE);
    (void)esp_gsp_keyboard_attach(
        ui, GSP_ACT_ID_SETTINGS_WLAN_PASSWORD_KEYBOARD_KEY,
        GSP_BIND_SETTINGS_WLAN_PASSWORD_VALUE);
}

static void settings_wlan_password_detach(esp_gsp_handle_t ui)
{
    /* Close the logical input session before touching framework state. A key
     * release already queued by the previous frame must not submit or edit
     * after Cancel/Join starts navigating away. */
    s_state.wlan_password_active = false;
    mosaic_app_shell_set_bottom_enabled(ui, true);
    mosaic_top_notice_hide(ui);
    (void)esp_gsp_set_cursor(ui, ESP_GSP_NO_CURSOR);
    (void)esp_gsp_keyboard_attach(
        ui, ESP_GSP_KEYBOARD_NONE, GSP_BIND_SETTINGS_WLAN_PASSWORD_VALUE);
    /* Clear the retained scene value while this page is still current. The
     * next StackView push may snapshot the password page before attach(), so
     * leaving the bind populated causes the previous password to flash. */
    (void)esp_gsp_set_text(
        ui, GSP_BIND_SETTINGS_WLAN_PASSWORD_VALUE, "");
    s_state.wlan.password[0] = '\0';
    s_state.wlan.selected_ssid[0] = '\0';
}

static uint32_t settings_wlan_network_index(const mosaic_event_t *event)
{
    if (event != NULL && event->data.call.list != ESP_GSP_LIST_NONE) {
        return event->data.call.item;
    }
    return event != NULL ? event->data.call.arg : 0U;
}

static esp_gsp_err_t settings_wlan_loading_frame_set_visible(
    esp_gsp_handle_t ui, uint8_t frame, bool visible)
{
    switch (frame) {
    case 0:
        return gsp_settings_settings_wlan_current_loading_0_set_visible(
            ui, visible);
    case 1:
        return gsp_settings_settings_wlan_current_loading_1_set_visible(
            ui, visible);
    case 2:
        return gsp_settings_settings_wlan_current_loading_2_set_visible(
            ui, visible);
    case 3:
        return gsp_settings_settings_wlan_current_loading_3_set_visible(
            ui, visible);
    case 4:
        return gsp_settings_settings_wlan_current_loading_4_set_visible(
            ui, visible);
    case 5:
        return gsp_settings_settings_wlan_current_loading_5_set_visible(
            ui, visible);
    case 6:
        return gsp_settings_settings_wlan_current_loading_6_set_visible(
            ui, visible);
    case 7:
        return gsp_settings_settings_wlan_current_loading_7_set_visible(
            ui, visible);
    default:
        return ESP_GSP_ERR_INVALID_ARG;
    }
}

static void settings_wlan_loading_timer_cb(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (ui != s_state.ui || !s_state.wlan_loading_visible) {
        return;
    }
    const uint8_t previous = s_state.wlan_loading_frame;
    const uint8_t next =
        (uint8_t)((previous + 1U) % SETTINGS_WLAN_LOADING_FRAME_COUNT);
    (void)settings_wlan_loading_frame_set_visible(ui, previous, false);
    (void)settings_wlan_loading_frame_set_visible(ui, next, true);
    s_state.wlan_loading_frame = next;
}

static esp_gsp_err_t settings_wlan_set_loading(
    esp_gsp_handle_t ui, bool visible)
{
    if (visible == s_state.wlan_loading_visible) {
        return ESP_GSP_OK;
    }
    if (visible) {
        s_state.wlan_loading_frame = 0;
        esp_gsp_err_t err = settings_wlan_loading_frame_set_visible(
            ui, 0, true);
        if (err != ESP_GSP_OK) {
            return err;
        }
        s_state.wlan_loading_timer = esp_gsp_timer_create(
            ui, SETTINGS_WLAN_LOADING_PERIOD_MS,
            settings_wlan_loading_timer_cb, NULL);
        if (s_state.wlan_loading_timer == NULL) {
            (void)settings_wlan_loading_frame_set_visible(ui, 0, false);
            return ESP_GSP_ERR_NO_MEM;
        }
        s_state.wlan_loading_visible = true;
        return ESP_GSP_OK;
    }
    if (s_state.wlan_loading_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_state.wlan_loading_timer);
        s_state.wlan_loading_timer = NULL;
    }
    esp_gsp_err_t err = settings_wlan_loading_frame_set_visible(
        ui, s_state.wlan_loading_frame, false);
    s_state.wlan_loading_visible = false;
    s_state.wlan_loading_frame = 0;
    return err;
}

static esp_err_t settings_wlan_render(esp_gsp_handle_t ui)
{
    const settings_wlan_state_t *wlan = &s_state.wlan;
    const mosaic_settings_network_t *network = &s_state.snapshot.network;
    const bool page_enabled = s_state.wlan_page_active && wlan->enabled;
    const bool show_current = settings_wlan_show_current();
    const char *status = TR("Not Connected", "未连接");
    if (s_state.wlan_connect_pending && !wlan->connected) {
        /* Provider setup can briefly publish IDLE while credentials are
         * persisted and the STA is restarted. Keep the user-facing operation
         * in Joining until a terminal success/failure event arrives. */
        status = TR("Joining...", "加入中...");
    } else switch (wlan->state) {
    case MOSAIC_SETTINGS_WIFI_CONNECTING:
        status = TR("Joining...", "加入中...");
        break;
    case MOSAIC_SETTINGS_WIFI_CONNECTED:
        status = TR("Current Network", "当前网络");
        break;
    case MOSAIC_SETTINGS_WIFI_RETRY_WAIT:
        status = TR("Waiting to reconnect", "等待重新连接");
        break;
    case MOSAIC_SETTINGS_WIFI_AUTH_FAILED:
        status = TR("Incorrect password", "密码错误");
        break;
    case MOSAIC_SETTINGS_WIFI_AP_NOT_FOUND:
        status = TR("Network not found", "未找到网络");
        break;
    case MOSAIC_SETTINGS_WIFI_FAILED:
        status = TR("Connection failed", "连接失败");
        break;
    default:
        break;
    }
    esp_err_t result = ESP_OK;
    const char *mode = TR("Off", "关");
    if (strcmp(network->mode, "provision") == 0) {
        mode = "AP";
    } else if (strcmp(network->mode, "apsta") == 0) {
        mode = "AP+STA";
    } else if (strcmp(network->mode, "sta_only") == 0) {
        mode = "STA";
    }
    char mode_info[64];
    if (network->ap_active && network->ap_ssid[0] != '\0') {
        if (mosaic_i18n_is_chinese()) {
            (void)snprintf(mode_info, sizeof(mode_info), "模式：%s · AP：%s",
                           mode, network->ap_ssid);
        } else {
            (void)snprintf(mode_info, sizeof(mode_info), "Mode: %s · AP: %s",
                           mode, network->ap_ssid);
        }
    } else {
        if (mosaic_i18n_is_chinese()) {
            (void)snprintf(mode_info, sizeof(mode_info), "模式：%s", mode);
        } else {
            (void)snprintf(mode_info, sizeof(mode_info), "Mode: %s", mode);
        }
    }

    keep_first_error(&result,
        gsp_settings_settings_wlan_enabled_set_checked(ui, wlan->enabled));
    keep_first_error(&result,
        gsp_settings_settings_wlan_mode_info_set_text(ui, mode_info));
    keep_first_error(&result,
        gsp_settings_settings_wlan_off_layer_set_visible(
            ui, false));
    keep_first_error(&result,
        gsp_settings_settings_wlan_list_layer_set_visible(
            ui, page_enabled));
    keep_first_error(&result,
        gsp_settings_settings_wlan_current_layer_set_visible(
            ui, show_current));
    keep_first_error(&result,
        gsp_settings_settings_wlan_current_check_set_visible(
            ui, show_current && wlan->connected));
    keep_first_error(&result, settings_wlan_set_loading(
        ui, show_current && !wlan->connected));
    keep_first_error(&result,
        gsp_settings_settings_wlan_current_error_set_visible(
            ui, false));
    keep_first_error(&result, settings_wlan_apply_layout(ui));
    keep_first_error(&result,
        gsp_settings_settings_wlan_current_status_set_text(ui, status));
    if (wlan->current_ssid[0] != '\0') {
        keep_first_error(&result,
            gsp_settings_settings_wlan_current_ssid_set_text(
                ui, wlan->current_ssid));
    } else {
        keep_first_error(&result,
            gsp_settings_settings_wlan_current_ssid_set_text(ui, "--"));
    }
    if (wlan->connected && wlan->current_ssid[0] != '\0') {
        keep_first_error(&result,
            gsp_settings_settings_wlan_detail_ssid_set_text(
                ui, wlan->current_ssid));
    } else {
        keep_first_error(&result,
            gsp_settings_settings_wlan_detail_ssid_set_text(
                ui, TR("Network", "网络")));
    }
    keep_first_error(&result,
        gsp_settings_settings_wlan_auto_join_set_checked(
            ui, wlan->auto_join));
    if (wlan->selected_ssid[0] != '\0') {
        keep_first_error(&result,
            gsp_settings_settings_wlan_password_ssid_set_text(
                ui, wlan->selected_ssid));
    }
    settings_wlan_update_summary(ui);
    if (page_enabled) {
        settings_wlan_refresh_list(ui);
    }
    return result;
}

static void settings_wlan_refresh_status(esp_gsp_handle_t ui)
{
    if (s_state.ops.get_wifi_status == NULL) {
        return;
    }
    mosaic_settings_network_t network = s_state.snapshot.network;
    if (s_state.ops.get_wifi_status(
            s_state.ops.user_ctx, &network) != ESP_OK) {
        return;
    }
    const bool was_enabled = s_state.wlan.enabled;
    const mosaic_settings_wifi_radio_state_t previous_radio =
        s_state.snapshot.network.radio_state;
    const bool host_terminal_without_operation =
        network.operation_id == 0U &&
        s_state.wlan_connect_operation_id == 0U &&
        (network.state == MOSAIC_SETTINGS_WIFI_CONNECTED ||
         network.state == MOSAIC_SETTINGS_WIFI_AUTH_FAILED ||
         network.state == MOSAIC_SETTINGS_WIFI_AP_NOT_FOUND ||
         network.state == MOSAIC_SETTINGS_WIFI_FAILED);
    const bool stale_connect_snapshot = s_state.wlan_connect_pending &&
        network.operation_id == s_state.wlan_connect_operation_id &&
        !host_terminal_without_operation;
    const bool presentation_changed =
        !stale_connect_snapshot &&
        (s_state.snapshot.network.desired_enabled != network.desired_enabled ||
         s_state.snapshot.network.connected != network.connected ||
         s_state.snapshot.network.ap_active != network.ap_active ||
         s_state.snapshot.network.state != network.state ||
         strcmp(s_state.snapshot.network.ssid, network.ssid) != 0 ||
         strcmp(s_state.snapshot.network.ap_ssid, network.ap_ssid) != 0 ||
         strcmp(s_state.snapshot.network.mode, network.mode) != 0);
    s_state.snapshot.network = network;
    s_state.wlan.enabled = network.desired_enabled;
    if (!stale_connect_snapshot) {
        s_state.wlan.connected = network.connected;
        s_state.wlan.state = network.state;
    }
    /* Saving credentials may restart Wi-Fi through an unconfigured/IDLE
     * snapshot whose SSID is temporarily empty. Preserve the selected target
     * while the connection transaction is pending so the Connecting card
     * does not blink out before the terminal event. */
    if (!s_state.wlan_connect_pending ||
            (network.operation_id != s_state.wlan_connect_operation_id &&
             network.ssid[0] != '\0')) {
        strlcpy(s_state.wlan.current_ssid, network.ssid,
                sizeof(s_state.wlan.current_ssid));
    }
    const int64_t now = esp_timer_get_time();
    if (network.scan_revision != s_state.wlan_scan_revision) {
        s_state.wlan_scan_revision = network.scan_revision;
        s_state.wlan_scan_waiting = false;
        s_state.wlan_scan_next_us = now +
            (network.scan_error == ESP_OK
                ? SETTINGS_WLAN_SCAN_REFRESH_US
                : SETTINGS_WLAN_SCAN_RETRY_US);
        if (network.scan_error == ESP_OK) {
            settings_wlan_scan(ui);
        }
    }
    if (!s_state.wlan.enabled) {
        s_state.wlan_scan_waiting = false;
        s_state.wlan_scan_next_us = 0;
        if (s_state.wlan_page_active) {
            settings_wlan_list_park(ui);
        }
    } else if (s_state.wlan_page_active &&
            (!was_enabled ||
             (previous_radio != MOSAIC_SETTINGS_WIFI_RADIO_ON &&
              network.radio_state == MOSAIC_SETTINGS_WIFI_RADIO_ON))) {
        s_state.wlan_scan_next_us = now;
        settings_wlan_list_attach(ui);
    }
    bool stack_animating = false;
    const bool transition_owns_snapshot = s_state.wlan_page_active &&
        esp_gsp_stack_view_is_animating(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &stack_animating) == ESP_GSP_OK &&
        stack_animating;
    /* Keep authored objects unchanged while StackView owns a viewport
     * snapshot. Device Wi-Fi events can otherwise mutate only the bound lower
     * layer midway through the transition and leave a half-page image. */
    if (presentation_changed && !transition_owns_snapshot) {
        (void)settings_wlan_render(ui);
    }
    settings_wlan_apply_phone_status(ui, &network);
    if (s_state.wlan_connect_pending &&
            (network.operation_id != s_state.wlan_connect_operation_id ||
             host_terminal_without_operation)) {
        const char *title = NULL;
        const char *message = NULL;
        switch (network.state) {
        case MOSAIC_SETTINGS_WIFI_CONNECTED:
            if (network.connected) {
                s_state.wlan_connect_pending = false;
                s_state.wlan_connect_operation_id = network.operation_id;
                s_state.wlan_scan_next_us = now;
            }
            break;
        case MOSAIC_SETTINGS_WIFI_AUTH_FAILED:
            title = TR("Incorrect password", "密码错误");
            message = TR("Check the password and try again", "请检查密码后重试");
            s_state.wlan_connect_pending = false;
            s_state.wlan_connect_operation_id = network.operation_id;
            if (s_state.ops.forget_wifi != NULL) {
                (void)s_state.ops.forget_wifi(s_state.ops.user_ctx);
            }
            s_state.wlan.connected = false;
            s_state.wlan.current_ssid[0] = '\0';
            s_state.snapshot.network.connected = false;
            s_state.snapshot.network.configured = false;
            s_state.snapshot.network.ssid[0] = '\0';
            break;
        case MOSAIC_SETTINGS_WIFI_AP_NOT_FOUND:
            title = TR("Network unavailable", "网络不可用");
            message = TR("The selected WLAN was not found", "未找到所选 WLAN");
            s_state.wlan_connect_pending = false;
            s_state.wlan_connect_operation_id = network.operation_id;
            break;
        case MOSAIC_SETTINGS_WIFI_FAILED:
            title = TR("Connection failed", "连接失败");
            message = TR("Check the network and try again", "请检查网络后重试");
            s_state.wlan_connect_pending = false;
            s_state.wlan_connect_operation_id = network.operation_id;
            break;
        case MOSAIC_SETTINGS_WIFI_DISABLED:
            s_state.wlan_connect_pending = false;
            s_state.wlan_connect_operation_id = network.operation_id;
            break;
        default:
            break;
        }
        if (!s_state.wlan_connect_pending) {
            s_state.wlan_connect_deadline_us = 0;
        }
        if (title != NULL && s_state.wlan_page_active) {
            s_state.wlan_scan_next_us = now;
            (void)settings_wlan_render(ui);
            /* Rendering the compact List and publishing the provider's
             * follow-up IDLE state can fill the device update queue in this
             * frame. Keep the terminal failure latched and submit the notice
             * by itself from a stable UI tick; SDL usually has enough room,
             * which previously hid this device-only loss. */
            s_state.wlan_notice_title = title;
            s_state.wlan_notice_message = message;
            s_state.wlan_notice_pending = true;
        }
    }
}

static bool settings_wlan_show_pending_notice(esp_gsp_handle_t ui)
{
    if (!s_state.wlan_notice_pending || !s_state.wlan_page_active ||
            s_state.wlan_password_active || s_state.wlan_leave_pending) {
        return false;
    }
    bool animating = false;
    if (esp_gsp_stack_view_is_animating(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &animating) != ESP_GSP_OK ||
            animating) {
        return false;
    }
    if (mosaic_top_notice_show(
            ui, &s_top_notice, s_state.wlan_notice_title,
            s_state.wlan_notice_message,
            SETTINGS_NOTICE_DURATION_MS) == ESP_GSP_OK) {
        s_state.wlan_notice_pending = false;
        s_state.wlan_notice_title = NULL;
        s_state.wlan_notice_message = NULL;
        return true;
    }
    return false;
}

static void settings_wlan_prepare_password(const char *ssid)
{
    strlcpy(s_state.wlan.selected_ssid, ssid,
            sizeof(s_state.wlan.selected_ssid));
}

static void settings_wlan_fake_connect(esp_gsp_handle_t ui, const char *ssid)
{
    strlcpy(s_state.wlan.current_ssid, ssid,
            sizeof(s_state.wlan.current_ssid));
    s_state.wlan.connected = true;
    s_state.wlan.state = MOSAIC_SETTINGS_WIFI_CONNECTED;
    s_state.wlan.selected_ssid[0] = '\0';
    (void)settings_wlan_render(ui);
    settings_wlan_refresh_list(ui);
}

static void settings_wlan_connection_started(
    esp_gsp_handle_t ui, const char *ssid)
{
    s_state.wlan_connect_pending = true;
    s_state.wlan_connect_deadline_us = esp_timer_get_time() +
        SETTINGS_WLAN_CONNECT_TIMEOUT_US;
    s_state.wlan.connected = false;
    s_state.wlan.state = MOSAIC_SETTINGS_WIFI_CONNECTING;
    strlcpy(s_state.wlan.current_ssid, ssid,
            sizeof(s_state.wlan.current_ssid));
    (void)settings_wlan_render(ui);
}

static void settings_wlan_pop_to_wlan(esp_gsp_handle_t ui)
{
    /* Each UI path calls this exactly once. StackView queues the command when
     * invoked from an input callback and applies it at the next frame. */
    if (esp_gsp_stack_view_pop(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, false) != ESP_GSP_OK) {
        ESP_LOGW(TAG, "return from WLAN password page failed");
    } else if (s_state.wlan_page_active && s_state.wlan.enabled) {
        settings_wlan_list_attach(ui);
    }
}

static void settings_wlan_cancel_password(esp_gsp_handle_t ui)
{
    if (!s_state.wlan_password_active) {
        return;
    }
    settings_wlan_password_detach(ui);
    settings_wlan_pop_to_wlan(ui);
}

static void settings_wlan_finish_leave(esp_gsp_handle_t ui)
{
    s_state.wlan_page_active = false;
    s_state.wlan_entry_pending = false;
    s_state.wlan_leave_pending = false;
    s_state.wlan_detail_active = false;
    s_state.wlan_detail_leave_pending = false;
    s_state.wlan_forget_pending = false;
    /* StackView/List owns page visibility. Do not rewrite the now-offscreen
     * WLAN bindings here: on retained/partition displays those incremental
     * writes can race the landing frame and preserve half of the old page.
     * The bindings are refreshed before the next WLAN push. */
    settings_wlan_list_park(ui);
    mosaic_app_shell_sync(ui);
}

static bool settings_wlan_reconcile_navigation(esp_gsp_handle_t ui)
{
    if (!s_state.wlan_page_active) {
        return false;
    }
    uint16_t page = UINT16_MAX;
    bool animating = false;
    if (esp_gsp_stack_view_get_top(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &page) != ESP_GSP_OK ||
            esp_gsp_stack_view_is_animating(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &animating) != ESP_GSP_OK) {
        return false;
    }
    if (s_state.wlan_phone_exit_pending &&
            page == SETTINGS_WLAN_PASSWORD_PAGE && !animating) {
        if (esp_gsp_stack_view_pop(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, false) == ESP_GSP_OK) {
            s_state.wlan_phone_exit_pending = false;
            s_state.wlan_phone_active = false;
            s_state.wlan_phone_operation_id = 0;
        }
        return true;
    }
    if (page == SETTINGS_WLAN_PAGE && !animating) {
        s_state.wlan_entry_pending = false;
        if (s_state.wlan_detail_leave_pending ||
                s_state.wlan_forget_pending) {
            s_state.wlan_detail_active = false;
            s_state.wlan_detail_leave_pending = false;
            s_state.wlan_forget_pending = false;
            if (s_state.wlan.enabled) {
                settings_wlan_list_attach(ui);
            }
            (void)settings_wlan_render(ui);
        }
    } else if (page == SETTINGS_WLAN_DETAIL_PAGE) {
        s_state.wlan_detail_active = true;
    }
    if (s_state.wlan_leave_pending && page == 0 && !animating) {
        settings_wlan_finish_leave(ui);
        return true;
    }
    return false;
}

static void settings_wlan_open_detail(esp_gsp_handle_t ui)
{
    if (s_state.wlan_detail_active ||
            s_state.wlan_detail_leave_pending ||
            s_state.wlan_forget_pending ||
            !s_state.wlan.connected ||
            s_state.wlan.current_ssid[0] == '\0') {
        return;
    }
    /* Claim the detail transaction before queueing. Several overlapping
     * visuals intentionally share this callback, and device touch can also
     * report a duplicate release before StackView updates its top. */
    s_state.wlan_detail_active = true;
    settings_wlan_list_park(ui);
    (void)settings_wlan_render(ui);
    if (esp_gsp_stack_view_push(
            ui, GSP_OBJ_KEY_SETTINGS_STACK,
            SETTINGS_WLAN_DETAIL_PAGE, true) != ESP_GSP_OK) {
        s_state.wlan_detail_active = false;
        settings_wlan_list_attach(ui);
    }
}

static void settings_wlan_handle_network_select(
    esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    /* Overlapping row visuals can deliver the same release more than once
     * before StackView reports its new top. Claim the input session before
     * pushing so one SSID tap cannot add the password page twice. */
    if (s_state.wlan_password_active) {
        return;
    }
    const uint32_t index = settings_wlan_network_index(event);
    if (index >= s_wlan_network_count) {
        return;
    }
    const char *ssid = s_wlan_networks[index].ssid;
    if (s_state.wlan.connected &&
            strcmp(s_state.wlan.current_ssid, ssid) == 0) {
        settings_wlan_open_detail(ui);
        return;
    }
    settings_wlan_prepare_password(ssid);
    if (!s_wlan_networks[index].secured) {
        if (s_state.ops.connect_wifi == NULL) {
            settings_wlan_fake_connect(ui, ssid);
        } else {
            s_state.wlan_connect_operation_id =
                s_state.snapshot.network.operation_id;
            if (s_state.ops.connect_wifi(
                    s_state.ops.user_ctx, ssid, "") == ESP_OK) {
                settings_wlan_connection_started(ui, ssid);
                settings_wlan_refresh_status(ui);
            } else {
                (void)mosaic_top_notice_show(
                    ui, &s_top_notice, TR("Connection failed", "连接失败"),
                    TR("Unable to start the connection", "无法启动连接"),
                    SETTINGS_NOTICE_DURATION_MS);
            }
        }
        return;
    }
    /* Queue the empty value before StackView captures the incoming page.
     * attach() intentionally runs after push and only activates input. */
    (void)esp_gsp_set_text(
        ui, GSP_BIND_SETTINGS_WLAN_PASSWORD_VALUE, "");
    (void)settings_wlan_render(ui);
    settings_wlan_list_park(ui);
    esp_gsp_err_t push_err = esp_gsp_stack_view_push(
            ui, GSP_OBJ_KEY_SETTINGS_STACK,
            SETTINGS_WLAN_PASSWORD_PAGE, true);
    if (push_err != ESP_GSP_OK) {
        ESP_LOGW(TAG, "open WLAN password page failed");
        settings_wlan_list_attach(ui);
        return;
    }
    settings_wlan_password_attach(ui);
}

static bool settings_wlan_handle_connect(esp_gsp_handle_t ui)
{
    if (!s_state.wlan_password_active ||
            s_state.wlan.selected_ssid[0] == '\0') {
        return false;
    }
    char password[sizeof(s_state.wlan.password)] = {0};
    if (esp_gsp_keyboard_text(ui, password, sizeof(password)) != ESP_GSP_OK) {
        return false;
    }
    if (strlen(password) < SETTINGS_WLAN_PASSWORD_MIN) {
        (void)mosaic_top_notice_show(
            ui, &s_top_notice, TR("Password too short", "密码过短"),
            TR("Enter at least 8 characters", "至少输入 8 个字符"),
            SETTINGS_NOTICE_DURATION_MS);
        return false;
    }
    char ssid[sizeof(s_state.wlan.selected_ssid)];
    strlcpy(ssid, s_state.wlan.selected_ssid, sizeof(ssid));
    strlcpy(s_state.wlan.password, password, sizeof(s_state.wlan.password));
    const bool simulated = s_state.ops.connect_wifi == NULL;
    s_state.wlan_connect_pending = !simulated;
    s_state.wlan_connect_operation_id =
        s_state.snapshot.network.operation_id;
    if (!simulated && s_state.ops.connect_wifi(
            s_state.ops.user_ctx, ssid, password) != ESP_OK) {
        s_state.wlan_connect_pending = false;
        ESP_LOGW(TAG, "connect WLAN %s failed", ssid);
        (void)mosaic_top_notice_show(
            ui, &s_top_notice, TR("Connection failed", "连接失败"),
            TR("Unable to start the connection", "无法启动连接"),
            SETTINGS_NOTICE_DURATION_MS);
        return false;
    }
    settings_wlan_password_detach(ui);
    if (simulated) {
        settings_wlan_fake_connect(ui, ssid);
    } else {
        settings_wlan_connection_started(ui, ssid);
        /* Providers may complete synchronously (the SDL host intentionally
         * does). Consume that terminal state before returning from Join;
         * asynchronous device providers simply report CONNECTING/IDLE here
         * and deliver their terminal event through the normal model path. */
        settings_wlan_refresh_status(ui);
    }
    return true;
}

static void settings_wlan_complete_phone_setup(esp_gsp_handle_t ui)
{
    if (!s_state.wlan_phone_active || s_state.wlan_phone_exit_pending) {
        return;
    }
    /* The phone page is nested above the password page. Pop one level now;
     * reconciliation pops the password page after this transition settles. */
    s_state.wlan_phone_exit_pending = true;
    if (esp_gsp_stack_view_pop(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, false) != ESP_GSP_OK) {
        s_state.wlan_phone_exit_pending = false;
        return;
    }
    settings_wlan_request_scan();
}

static void settings_wlan_apply_phone_status(
    esp_gsp_handle_t ui, const mosaic_settings_network_t *network)
{
    if (!s_state.wlan_phone_active || s_state.wlan_phone_exit_pending ||
            network == NULL ||
            network->operation_id == s_state.wlan_phone_operation_id) {
        return;
    }

    const char *title = NULL;
    const char *message = NULL;
    switch (network->state) {
    case MOSAIC_SETTINGS_WIFI_CONNECTED:
        if (network->connected) {
            settings_wlan_complete_phone_setup(ui);
        }
        return;
    case MOSAIC_SETTINGS_WIFI_AUTH_FAILED:
        title = TR("Incorrect password", "密码错误");
        message = TR("Update the password on your phone", "请在手机上更新密码");
        break;
    case MOSAIC_SETTINGS_WIFI_AP_NOT_FOUND:
        title = TR("Network unavailable", "网络不可用");
        message = TR("Choose another WLAN on your phone", "请在手机上选择其他 WLAN");
        break;
    case MOSAIC_SETTINGS_WIFI_FAILED:
        title = TR("Connection failed", "连接失败");
        message = TR("Check the settings and try again", "请检查设置后重试");
        break;
    default:
        return;
    }

    /* Consume this failed portal submission exactly once. A later retry
     * receives a new Wi-Fi manager operation id and is evaluated normally. */
    s_state.wlan_phone_operation_id = network->operation_id;
    s_state.wlan_notice_title = title;
    s_state.wlan_notice_message = message;
    s_state.wlan_notice_pending = true;
}

static void settings_wlan_handle_phone_submitted(esp_gsp_handle_t ui)
{
    settings_wlan_refresh_status(ui);
    if (!s_state.wlan_phone_active || s_state.wlan_phone_exit_pending) {
        return;
    }
    if (s_state.wlan_notice_pending) {
        (void)settings_wlan_show_pending_notice(ui);
    } else {
        (void)mosaic_top_notice_show(
            ui, &s_top_notice, TR("Waiting for phone", "等待手机"),
            TR("Submit Wi-Fi settings in the portal", "请在门户中提交 Wi-Fi 设置"),
            SETTINGS_NOTICE_DURATION_MS);
    }
}

static void settings_wlan_handle_forget(esp_gsp_handle_t ui)
{
    if (s_state.wlan_forget_pending) {
        return;
    }
    uint16_t page = 0;
    if (esp_gsp_stack_view_get_top(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &page) != ESP_GSP_OK ||
            page != SETTINGS_WLAN_DETAIL_PAGE) {
        return;
    }
    /* A hardware release can enqueue the same click again before the
     * asynchronous StackView pop has changed its reported top. Latch the
     * navigation before calling the provider so exactly one pop is queued. */
    s_state.wlan_forget_pending = true;
    if (s_state.ops.forget_wifi != NULL &&
            s_state.ops.forget_wifi(s_state.ops.user_ctx) != ESP_OK) {
        ESP_LOGW(TAG, "forget WLAN failed");
        s_state.wlan_forget_pending = false;
        return;
    }
    s_state.wlan.connected = false;
    s_state.wlan.current_ssid[0] = '\0';
    s_state.wlan.selected_ssid[0] = '\0';
    s_state.snapshot.network.connected = false;
    s_state.snapshot.network.ssid[0] = '\0';
    (void)settings_wlan_render(ui);
    if (esp_gsp_stack_view_pop(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, false) != ESP_GSP_OK) {
        s_state.wlan_forget_pending = false;
    }
}

static esp_err_t settings_render_web_logs(esp_gsp_handle_t ui)
{
    bool enabled = s_state.snapshot.web_logs_enabled;
    esp_err_t result = esp_gsp_set_visible(
        ui, GSP_BIND_SETTINGS_WEB_LOGS_ON_VISIBLE, enabled);
    keep_first_error(&result, esp_gsp_set_visible(
        ui, GSP_BIND_SETTINGS_WEB_LOGS_OFF_VISIBLE, !enabled));
    return result;
}

static esp_err_t settings_render_snapshot(esp_gsp_handle_t ui)
{
    const mosaic_settings_snapshot_t *snapshot = &s_state.snapshot;
    char text[48];
    esp_err_t result = ESP_OK;
    s_state.wlan.enabled = snapshot->network.desired_enabled;
    const bool stale_connect_snapshot = s_state.wlan_connect_pending &&
        snapshot->network.operation_id ==
            s_state.wlan_connect_operation_id;
    if (!stale_connect_snapshot) {
        s_state.wlan.connected = snapshot->network.connected;
        s_state.wlan.state = snapshot->network.state;
        strlcpy(s_state.wlan.current_ssid, snapshot->network.ssid,
                sizeof(s_state.wlan.current_ssid));
    }
    if (!s_state.brightness_drag_active) {
        s_state.brightness = snapshot->brightness;
    }
    if (!s_state.volume_drag_active) {
        s_state.volume = snapshot->volume;
    }
    settings_display_request_render();

    keep_first_error(&result,
        settings_wlan_render(ui));
    keep_first_error(&result,
        settings_render_web_logs(ui));

    (void)snprintf(text, sizeof(text), "v%u.%u.%u",
                   GSP_VERSION_MAJOR, GSP_VERSION_MINOR, GSP_VERSION_PATCH);
    keep_first_error(&result, esp_gsp_set_text(
        ui, GSP_BIND_SETTINGS_ABOUT_GSP_VERSION, text));

    return result;
}

static void settings_refresh_snapshot(esp_gsp_handle_t ui)
{
    if (s_state.ops.get_snapshot == NULL) {
        return;
    }
    mosaic_settings_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL) {
        ESP_LOGE(TAG, "allocate Settings snapshot failed");
        return;
    }
    esp_err_t err = s_state.ops.get_snapshot(
        s_state.ops.user_ctx, snapshot);
    if (err == ESP_OK) {
        s_state.snapshot = *snapshot;
        err = settings_render_snapshot(ui);
    }
    free(snapshot);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "refresh Settings snapshot failed: %s",
                 esp_err_to_name(err));
    }
}

static bool settings_detail_visible(
    esp_gsp_handle_t ui, settings_detail_kind_t kind)
{
    uint16_t top = 0U;
    return s_detail_kind == kind &&
        esp_gsp_stack_view_get_top(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &top) == ESP_GSP_OK &&
        top == SETTINGS_DETAIL_PAGE;
}

static void settings_refresh_about_detail(esp_gsp_handle_t ui)
{
    if (settings_detail_visible(ui, SETTINGS_DETAIL_ABOUT) &&
            s_detail_list != ESP_GSP_LIST_NONE) {
        (void)esp_gsp_list_refresh(ui, s_detail_list);
    }
}

static void settings_refresh_battery_detail(esp_gsp_handle_t ui)
{
    if (s_state.ops.get_battery == NULL ||
            s_detail_list == ESP_GSP_LIST_NONE) {
        return;
    }
    mosaic_settings_battery_t battery = {0};
    esp_err_t err = s_state.ops.get_battery(
        s_state.ops.user_ctx, &battery);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "refresh Battery detail failed: %s",
                 esp_err_to_name(err));
        return;
    }
    s_state.snapshot.battery = battery;
    esp_gsp_err_t gsp_err = esp_gsp_list_refresh(
        ui, s_detail_list);
    if (gsp_err != ESP_GSP_OK) {
        ESP_LOGW(TAG, "queue Battery list refresh failed: 0x%x",
                 (unsigned)gsp_err);
    }
}

static const uint16_t s_integration_llm_progress[] = {
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_01,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_02,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_03,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_04,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_05,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_06,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_07,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_08,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_09,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_10,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_11,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_12,
};

static void settings_integration_progress(
    esp_gsp_handle_t ui, const uint16_t *binds, uint8_t count)
{
    if (count > SETTINGS_INTEGRATION_PROGRESS_COUNT) {
        count = SETTINGS_INTEGRATION_PROGRESS_COUNT;
    }
    for (uint8_t i = 0; i < SETTINGS_INTEGRATION_PROGRESS_COUNT; ++i) {
        (void)esp_gsp_set_color(ui, binds[i],
            i < count ? SETTINGS_INTEGRATION_PROGRESS_ON
                      : SETTINGS_INTEGRATION_PROGRESS_OFF);
    }
}

static void *settings_qr_alloc(size_t bytes)
{
#if defined(ESP_PLATFORM)
    void *memory = heap_caps_malloc(bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : heap_caps_malloc(
        bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}

static bool settings_qr_prepare(void)
{
    if (s_state.qr_temp == NULL) {
        s_state.qr_temp = settings_qr_alloc(qrcodegen_BUFFER_LEN_MAX);
    }
    if (s_state.qr_code == NULL) {
        s_state.qr_code = settings_qr_alloc(qrcodegen_BUFFER_LEN_MAX);
    }
    for (size_t i = 0; i < SETTINGS_INTEGRATION_QR_FRAMES; ++i) {
        if (s_state.qr_frames[i].pixels == NULL) {
            s_state.qr_frames[i].pixels = settings_qr_alloc(
                SETTINGS_INTEGRATION_QR_BYTES);
            atomic_store_explicit(&s_state.qr_frames[i].busy, false,
                                  memory_order_release);
        }
        if (s_state.qr_frames[i].pixels == NULL) return false;
    }
    return s_state.qr_temp != NULL && s_state.qr_code != NULL;
}

static bool settings_qr_free_buffers(void)
{
    uint8_t *frames[SETTINGS_INTEGRATION_QR_FRAMES] = {0};
    for (size_t i = 0; i < SETTINGS_INTEGRATION_QR_FRAMES; ++i) {
        if (atomic_load_explicit(
                &s_state.qr_frames[i].busy, memory_order_acquire)) {
            return false;
        }
    }

    uint8_t *temp = s_state.qr_temp;
    uint8_t *code = s_state.qr_code;
    s_state.qr_temp = NULL;
    s_state.qr_code = NULL;
    for (size_t i = 0; i < SETTINGS_INTEGRATION_QR_FRAMES; ++i) {
        frames[i] = s_state.qr_frames[i].pixels;
        s_state.qr_frames[i].pixels = NULL;
    }
    free(temp);
    free(code);
    for (size_t i = 0; i < SETTINGS_INTEGRATION_QR_FRAMES; ++i) {
        free(frames[i]);
    }
    return true;
}

static void settings_qr_release(void *user_ctx)
{
    settings_qr_frame_t *frame = user_ctx;
    if (frame != NULL) {
        atomic_store_explicit(&frame->busy, false, memory_order_release);
    }
}

static settings_qr_frame_t *settings_qr_acquire(void)
{
    for (size_t i = 0; i < SETTINGS_INTEGRATION_QR_FRAMES; ++i) {
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(
                &s_state.qr_frames[i].busy, &expected, true,
                memory_order_acq_rel, memory_order_acquire)) {
            return &s_state.qr_frames[i];
        }
    }
    return NULL;
}

static bool settings_qr_render(
    const char *payload, uint8_t *pixels, uint16_t size)
{
    if (payload == NULL || payload[0] == '\0' ||
            !qrcodegen_encodeText(payload, s_state.qr_temp, s_state.qr_code,
                qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        return false;
    }
    const int modules = qrcodegen_getSize(s_state.qr_code);
    const int extent = modules + SETTINGS_INTEGRATION_QR_QUIET * 2;
    const int scale = size / extent;
    if (scale < 1) {
        return false;
    }
    const int offset = (size - extent * scale) / 2;
    uint16_t *frame = (uint16_t *)pixels;
    for (size_t i = 0; i < (size_t)size * size; ++i) {
        frame[i] = UINT16_MAX;
    }
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (!qrcodegen_getModule(s_state.qr_code, x, y)) continue;
            const int px = offset + (x + SETTINGS_INTEGRATION_QR_QUIET) * scale;
            const int py = offset + (y + SETTINGS_INTEGRATION_QR_QUIET) * scale;
            for (int dy = 0; dy < scale; ++dy) {
                uint16_t *row = frame +
                    (size_t)(py + dy) * size;
                for (int dx = 0; dx < scale; ++dx) row[px + dx] = 0;
            }
        }
    }
    return true;
}

static void settings_qr_push(
    esp_gsp_handle_t ui, uint16_t bind, uint16_t size, const char *payload,
    char *rendered, size_t rendered_size)
{
    if (payload == NULL || payload[0] == '\0' ||
            strcmp(payload, rendered) == 0) return;
    if (!settings_qr_prepare()) {
        ESP_LOGW(TAG, "allocate QR buffers failed: bind=%u", (unsigned)bind);
        return;
    }
    settings_qr_frame_t *frame = settings_qr_acquire();
    if (frame == NULL) {
        ESP_LOGW(TAG, "no free QR frame: bind=%u", (unsigned)bind);
        return;
    }
    if (!settings_qr_render(payload, frame->pixels, size)) {
        settings_qr_release(frame);
        ESP_LOGW(TAG, "render QR failed: bind=%u", (unsigned)bind);
        return;
    }
    const esp_gsp_err_t err = esp_gsp_canvas_push(ui, bind, frame->pixels, size * 2U, settings_qr_release, frame);
    if (err != ESP_GSP_OK) {
        settings_qr_release(frame);
        ESP_LOGW(TAG, "push QR failed: bind=%u err=%d", (unsigned)bind, (int)err);
        return;
    }
    strlcpy(rendered, payload, rendered_size);
}

static void settings_wlan_phone_refresh(esp_gsp_handle_t ui)
{
    char ap_ssid[MOSAIC_SETTINGS_SSID_LEN] = {0};
    char *payload = calloc(1, MOSAIC_SETTINGS_PHONE_QR_LEN);
    if (payload == NULL) {
        return;
    }
    esp_err_t err = s_state.ops.get_phone_setup != NULL
        ? s_state.ops.get_phone_setup(
              s_state.ops.user_ctx, ap_ssid, sizeof(ap_ssid),
              payload, MOSAIC_SETTINGS_PHONE_QR_LEN)
        : ESP_ERR_NOT_SUPPORTED;
    if (err == ESP_OK) {
        (void)esp_gsp_set_text(
            ui, GSP_BIND_SETTINGS_WLAN_PHONE_AP_SSID, ap_ssid);
        settings_qr_push(
            ui, GSP_BIND_SETTINGS_WLAN_PHONE_QR_CANVAS,
            SETTINGS_WLAN_PHONE_QR_SIZE, payload,
            s_state.rendered_phone_qr,
            sizeof(s_state.rendered_phone_qr));
    } else {
        (void)esp_gsp_set_text(
            ui, GSP_BIND_SETTINGS_WLAN_PHONE_AP_SSID,
            TR("Provisioning AP unavailable", "配网热点不可用"));
        ESP_LOGW(TAG, "read phone provisioning QR failed: %s",
                 esp_err_to_name(err));
    }
    free(payload);
}

static void settings_llm_render(esp_gsp_handle_t ui)
{
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_STATUS_VISIBLE,
        s_state.llm_phase == SETTINGS_LLM_STATUS);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONFIGURING_VISIBLE,
        s_state.llm_phase == SETTINGS_LLM_CONFIGURING);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_VISIBLE,
        s_state.llm_phase == SETTINGS_LLM_PROGRESS);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_SUCCESS_VISIBLE,
        false);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_STATUS_BACK_VISIBLE, true);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_STATUS_SKIP_VISIBLE, false);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONFIGURING_BACK_VISIBLE, true);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONFIGURING_SKIP_VISIBLE, false);
}

static void settings_build_configuration_url(
    const mosaic_settings_snapshot_t *snapshot, const char *fragment,
    char *url, size_t url_size)
{
    if (snapshot == NULL || fragment == NULL || url == NULL ||
            url_size == 0U) {
        return;
    }
    if (snapshot->network.connected && snapshot->network.ip[0] != '\0') {
        (void)snprintf(url, url_size, "http://%s/#%s",
                       snapshot->network.ip, fragment);
        return;
    }

    const char *portal = snapshot->network.portal_url[0] != '\0'
        ? snapshot->network.portal_url : "http://192.168.4.1/";
    const size_t portal_len = strlen(portal);
    (void)snprintf(url, url_size, "%s%s#%s", portal,
                   portal_len > 0U && portal[portal_len - 1U] == '/' ? "" : "/",
                   fragment);
}

static void settings_build_connection_hint(
    const mosaic_settings_snapshot_t *snapshot, char *hint, size_t hint_size)
{
    if (snapshot == NULL || hint == NULL || hint_size == 0U) {
        return;
    }
    if (snapshot->network.connected && snapshot->network.ssid[0] != '\0') {
        (void)snprintf(hint, hint_size, "STA: %s",
                       snapshot->network.ssid);
        return;
    }
    if (s_state.provisioning_ap_ssid[0] == '\0' &&
            s_state.ops.get_phone_setup != NULL) {
        char *payload = calloc(1, MOSAIC_SETTINGS_PHONE_QR_LEN);
        if (payload != NULL) {
            (void)s_state.ops.get_phone_setup(
                s_state.ops.user_ctx,
                s_state.provisioning_ap_ssid,
                sizeof(s_state.provisioning_ap_ssid),
                payload, MOSAIC_SETTINGS_PHONE_QR_LEN);
            free(payload);
        }
    }
    (void)snprintf(hint, hint_size, "AP: %s",
                   s_state.provisioning_ap_ssid[0] != '\0'
                       ? s_state.provisioning_ap_ssid
                       : TR("unavailable", "不可用"));
}

static void settings_llm_refresh(esp_gsp_handle_t ui)
{
    settings_refresh_snapshot(ui);
    const mosaic_settings_snapshot_t *snapshot = &s_state.snapshot;
    (void)esp_gsp_set_text(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_BACKEND,
        snapshot->llm.backend[0] ? snapshot->llm.backend
                                 : TR("Not configured", "未配置"));
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_INTEGRATIONS_LLM_MODEL,
        snapshot->llm.model[0] ? snapshot->llm.model
                               : TR("Not configured", "未配置"));
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_INTEGRATIONS_LLM_BASE_URL,
        snapshot->llm.base_url[0] ? snapshot->llm.base_url
                                  : TR("Not configured", "未配置"));
    char capabilities[48];
    if (mosaic_i18n_is_chinese()) {
        (void)snprintf(capabilities, sizeof(capabilities), "工具 %s · 视觉 %s",
            snapshot->llm.supports_tools ? TR("On", "开") : TR("Off", "关"),
            snapshot->llm.supports_vision ? TR("On", "开") : TR("Off", "关"));
    } else {
        (void)snprintf(capabilities, sizeof(capabilities), "Tools %s · Vision %s",
            snapshot->llm.supports_tools ? "On" : "Off",
            snapshot->llm.supports_vision ? "On" : "Off");
    }
    (void)esp_gsp_set_text(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CAPABILITIES, capabilities);
    char url[MOSAIC_SETTINGS_LLM_URL_LEN] = {0};
    char connection_hint[MOSAIC_SETTINGS_SSID_LEN + 32U] = {0};
    settings_build_connection_hint(
        snapshot, connection_hint, sizeof(connection_hint));
    (void)esp_gsp_set_text(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONNECTION_HINT,
        connection_hint);
    settings_build_configuration_url(snapshot, "llm", url, sizeof(url));
    (void)esp_gsp_set_text(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONFIG_URL,
        url[0] ? url : TR("Unavailable", "不可用"));
    settings_qr_push(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONFIG_QR_CANVAS,
        SETTINGS_QR_SIZE, url,
        s_state.rendered_llm_qr, sizeof(s_state.rendered_llm_qr));
    /* Settings owns an information page, not Setup's completion ceremony.
     * A successful refresh returns directly to the retained status list so
     * the newly configured backend/model fields become visible immediately. */
    s_state.llm_phase = SETTINGS_LLM_STATUS;
    settings_llm_render(ui);
}

static void settings_channels_refresh(esp_gsp_handle_t ui)
{
    settings_refresh_snapshot(ui);
    const mosaic_settings_snapshot_t *snapshot = &s_state.snapshot;
    const bool configured[] = {
        snapshot->im.wechat_configured,
        snapshot->im.qq_configured,
        snapshot->im.feishu_configured,
        snapshot->im.telegram_configured,
    };
    const uint16_t on_binds[] = {
        GSP_BIND_SETTINGS_CHANNELS_WECHAT_ON_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_QQ_ON_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_FEISHU_ON_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_TELEGRAM_ON_VISIBLE,
    };
    const uint16_t off_binds[] = {
        GSP_BIND_SETTINGS_CHANNELS_WECHAT_OFF_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_QQ_OFF_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_FEISHU_OFF_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_TELEGRAM_OFF_VISIBLE,
    };
    for (size_t i = 0; i < sizeof(configured) / sizeof(configured[0]); ++i) {
        (void)esp_gsp_set_visible(ui, on_binds[i], configured[i]);
        (void)esp_gsp_set_visible(ui, off_binds[i], !configured[i]);
    }

    char url[MOSAIC_SETTINGS_LLM_URL_LEN] = {0};
    char connection_hint[MOSAIC_SETTINGS_SSID_LEN + 32U] = {0};
    settings_build_connection_hint(
        snapshot, connection_hint, sizeof(connection_hint));
    (void)esp_gsp_set_text(ui,
        GSP_BIND_SETTINGS_CHANNELS_CONNECTION_HINT, connection_hint);
    settings_build_configuration_url(snapshot, "im", url, sizeof(url));
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_CHANNELS_CONFIG_URL,
                           url[0] ? url : TR("Unavailable", "不可用"));
    settings_qr_push(ui, GSP_BIND_SETTINGS_CHANNELS_CONFIG_QR_CANVAS,
                     SETTINGS_CHANNEL_QR_SIZE, url,
                     s_state.rendered_channel_qr,
                     sizeof(s_state.rendered_channel_qr));
}

static void settings_integrations_pop(esp_gsp_handle_t ui)
{
    settings_refresh_snapshot(ui);
    settings_root_list_refresh(ui);
    (void)esp_gsp_stack_view_pop(ui, GSP_OBJ_KEY_SETTINGS_STACK, false);
    mosaic_app_shell_set_root_visible(ui, true);
    s_state.integration_page = 0;
}

static void settings_open_integration(esp_gsp_handle_t ui, bool llm)
{
    const uint16_t page = llm
        ? SETTINGS_INTEGRATIONS_MODEL_PAGE
        : SETTINGS_INTEGRATIONS_CHANNELS_PAGE;
    if (esp_gsp_stack_view_push(ui, GSP_OBJ_KEY_SETTINGS_STACK,
            page, true) != ESP_GSP_OK) {
        (void)mosaic_top_notice_show(
            ui, &s_top_notice, TR("AI & Integrations", "AI与集成"),
            TR("Unable to open page", "无法打开页面"),
            SETTINGS_NOTICE_DURATION_MS);
        return;
    }
    mosaic_app_shell_set_root_visible(ui, false);
    s_state.integration_page = page;
    if (llm) {
        s_state.rendered_llm_qr[0] = '\0';
        settings_llm_refresh(ui);
    } else {
        s_state.rendered_channel_qr[0] = '\0';
        settings_channels_refresh(ui);
    }
}

static void settings_dispatch_call(
    esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    switch (event->data.call.action_id) {
    case GSP_ACT_ID_SETTINGS_ROOT_ROW: {
        const uint32_t item = event->data.call.item;
        if (item >= sizeof(s_root_rows) / sizeof(s_root_rows[0])) {
            return;
        }
        if (item <= 1U) {
            settings_open_integration(ui, item == 0U);
            return;
        }
        if (item == 2U) {
            if (s_state.wlan_page_active) {
                return;
            }
            s_state.wlan_page_active = true;
            s_state.wlan_entry_pending = true;
            s_state.wlan_leave_pending = false;
            s_state.wlan_detail_active = false;
            s_state.wlan_detail_leave_pending = false;
            esp_gsp_err_t err = esp_gsp_stack_view_push(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, SETTINGS_WLAN_PAGE, true);
            if (err != ESP_GSP_OK) {
                err = esp_gsp_stack_view_push(
                    ui, GSP_OBJ_KEY_SETTINGS_STACK, SETTINGS_WLAN_PAGE, false);
            }
            if (err != ESP_GSP_OK) {
                s_state.wlan_page_active = false;
                s_state.wlan_entry_pending = false;
                ESP_LOGW(TAG, "open WLAN page failed: 0x%x",
                         (unsigned)err);
                return;
            }
            mosaic_app_shell_set_root_visible(ui, false);
            (void)settings_wlan_render(ui);
            s_state.wlan_scan_next_us = event->timestamp_us;
            settings_wlan_request_scan();
            settings_wlan_scan(ui);
            settings_wlan_list_attach(ui);
            return;
        }
        if (item == 6U) {
            settings_refresh_snapshot(ui);
            settings_open_detail(ui, SETTINGS_DETAIL_BATTERY);
            return;
        }
        if (item == SETTINGS_ROOT_ABOUT_INDEX) {
            settings_refresh_snapshot(ui);
            settings_open_detail(ui, SETTINGS_DETAIL_ABOUT);
            return;
        }
        if (item == SETTINGS_ROOT_LOCATION_INDEX) {
            settings_open_detail(ui, SETTINGS_DETAIL_CITY);
            return;
        }
        if (item == SETTINGS_ROOT_LANGUAGE_INDEX) {
            settings_open_detail(ui, SETTINGS_DETAIL_LANGUAGE);
            return;
        }
        static const uint16_t pages[] = {
            [3] = SETTINGS_DISPLAY_PAGE,
            [4] = SETTINGS_SOUND_PAGE,
            [5] = SETTINGS_SECURITY_PAGE,
            [SETTINGS_ROOT_DEBUG_INDEX] = SETTINGS_DEBUG_PAGE,
        };
        const uint16_t page = pages[item];
        if (page == 0U || esp_gsp_stack_view_push(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, page, true) != ESP_GSP_OK) {
            return;
        }
        mosaic_app_shell_set_root_visible(ui, false);
        if (item == 3U || item == 4U) {
            s_state.display_page_active = true;
            s_state.display_reset_pending = false;
            settings_display_request_render();
        }
        return;
    }
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_FLOW_BACK:
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_BACK:
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_CONTINUE:
        settings_integrations_pop(ui);
        return;
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_RECONFIGURE:
        s_state.llm_phase = SETTINGS_LLM_CONFIGURING;
        settings_llm_render(ui);
        return;
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_SUBMITTED:
        s_state.llm_phase = SETTINGS_LLM_PROGRESS;
        s_state.integration_started_us = event->timestamp_us;
        s_state.integration_deadline_us = event->timestamp_us +
            SETTINGS_INTEGRATION_CONFIG_US;
        s_state.integration_rendered_progress = UINT8_MAX;
        settings_integration_progress(ui, s_integration_llm_progress, 0);
        settings_llm_render(ui);
        return;
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_CANCEL:
        settings_llm_refresh(ui);
        return;
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_SKIP:
        settings_integrations_pop(ui);
        return;
    case GSP_ACT_ID_SETTINGS_ROTATION_SELECT: {
        const uint32_t index = event->data.call.list != ESP_GSP_LIST_NONE
            ? event->data.call.item : event->data.call.arg;
        if (index > 3U) {
            return;
        }
        const uint16_t degrees = (uint16_t)(index * 90U);
        esp_err_t err = s_state.ops.set_rotation(
            s_state.ops.user_ctx, degrees);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set rotation failed: %s", esp_err_to_name(err));
            settings_refresh_snapshot(ui);
            return;
        }
        s_state.snapshot.rotation = degrees;
        settings_display_request_render();
        return;
    }
    case GSP_ACT_ID_SETTINGS_SCREEN_TIMEOUT_SELECT: {
        static const uint32_t timeout_options_ms[] = {
            10000U, 30000U, 60000U, 120000U, 300000U, 0U,
        };
        const uint32_t index = event->data.call.list != ESP_GSP_LIST_NONE
            ? event->data.call.item : event->data.call.arg;
        if (index >= sizeof(timeout_options_ms) /
                     sizeof(timeout_options_ms[0])) {
            return;
        }
        const uint32_t timeout_ms = timeout_options_ms[index];
        esp_err_t err = mosaic_settings_set_screen_timeout(timeout_ms);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set screen timeout failed: %s", esp_err_to_name(err));
            settings_refresh_snapshot(ui);
            return;
        }
        s_state.snapshot.screen_timeout_ms = timeout_ms;
        settings_display_request_render();
        return;
    }
    case GSP_ACT_ID_SETTINGS_WLAN_LEAVE:
        if (!s_state.wlan_page_active || s_state.wlan_leave_pending ||
                s_state.wlan_detail_active ||
                s_state.wlan_password_active) {
            return;
        }
        /* A visible WLAN back target proves the queued entry has landed.
         * Claim this pop before submitting it so duplicate releases cannot
         * consume another StackView depth. */
        s_state.wlan_entry_pending = false;
        s_state.wlan_leave_pending = true;
        if (esp_gsp_stack_view_pop(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, false) != ESP_GSP_OK) {
            s_state.wlan_leave_pending = false;
            return;
        }
        mosaic_app_shell_set_root_visible(ui, true);
        return;
    case GSP_ACT_ID_SETTINGS_DISPLAY_PAGE:
        s_state.display_page_active = event->data.call.arg != 0;
        mosaic_app_shell_set_root_visible(
            ui, !s_state.display_page_active);
        if (s_state.display_page_active) {
            s_state.display_reset_pending = false;
            settings_display_request_render();
        } else {
            /* Do not reset while the exiting page remains visible; the
             * periodic callback performs it after the StackView pop has
             * completed. */
            s_state.display_reset_pending = true;
            s_state.display_pointer_pressed = false;
            s_state.display_pointer_scrolled = false;
            s_state.display_tap = SETTINGS_DISPLAY_TAP_NONE;
            s_state.brightness_drag_active = false;
            s_state.brightness_dirty = false;
            s_state.volume_drag_active = false;
            s_state.volume_dirty = false;
            settings_root_list_refresh(ui);
        }
        return;
    case GSP_ACT_ID_SETTINGS_DETAIL_ROW:
        if (s_detail_kind == SETTINGS_DETAIL_CITY) {
            const uint32_t item = event->data.call.item;
            if (s_city_province < 0) {
                /* Province list: row 0 clears the selection, other rows drill
                 * down into the province without popping the detail page. */
                if (item >= s_city_row_count) {
                    return;
                }
                if (item == 0U) {
                    if (mosaic_settings_set_weather_city(-1) != ESP_OK) {
                        mosaic_top_notice_show(ui, &s_top_notice,
                            TR("Location", "位置"),
                            TR("Unable to save city", "无法保存城市"),
                            SETTINGS_NOTICE_DURATION_MS);
                        return;
                    }
                    s_state.snapshot.weather_city_index = -1;
                    settings_city_render_list(ui);
                    return;
                }
                const size_t province_index = (size_t)item - 1U;
                if (province_index < mosaic_settings_province_count()) {
                    s_city_province = (int)province_index;
                    settings_city_render_list(ui);
                    (void)esp_gsp_list_scroll_to(ui, s_detail_list, 0);
                }
                return;
            }
            /* City list: map the row to its flat global preset index. */
            const mosaic_settings_province_t *province =
                mosaic_settings_province((size_t)s_city_province);
            if (province == NULL || item >= province->city_count) {
                return;
            }
            const int city_index =
                (int)settings_province_city_base((size_t)s_city_province) +
                (int)item;
            if (mosaic_settings_set_weather_city(city_index) != ESP_OK) {
                mosaic_top_notice_show(ui, &s_top_notice,
                    TR("Location", "位置"),
                    TR("Unable to save city", "无法保存城市"),
                    SETTINGS_NOTICE_DURATION_MS);
                return;
            }
            s_state.snapshot.weather_city_index = city_index;
            settings_city_render_list(ui);
            return;
        }
        if (s_detail_kind == SETTINGS_DETAIL_LANGUAGE) {
            const uint32_t item = event->data.call.item;
            if (item > 1U) {
                return;
            }
            const char *tag = item == 0U ? "en" : "zh";
            if (mosaic_settings_set_language(tag) != ESP_OK) {
                mosaic_top_notice_show(ui, &s_top_notice,
                    TR("Language", "语言"),
                    TR("Unable to save language", "无法保存语言设置"),
                    SETTINGS_NOTICE_DURATION_MS);
                return;
            }
            s_state.snapshot.language[0] = '\0';
            strlcpy(s_state.snapshot.language, tag,
                    sizeof(s_state.snapshot.language));
            settings_detail_list_attach(ui);
            (void)esp_gsp_list_refresh(ui, s_detail_list);
            return;
        }

        if (s_detail_kind == SETTINGS_DETAIL_SAFE_MODE &&
                event->data.call.item == 3U) {
            (void)mosaic_top_notice_show(
                ui, &s_top_notice, TR("Safe Mode", "安全模式"),
                TR("Restart required", "需要重启"),
                SETTINGS_NOTICE_DURATION_MS);
        }
        return;
    case GSP_ACT_ID_SETTINGS_WEB_LOGS_TOGGLE: {
        /* Each visible button requests an explicit state, so a duplicate
         * release cannot invert the setting a second time. */
        const bool enabled = event->data.call.arg != 0;
        if (enabled == s_state.snapshot.web_logs_enabled) {
            return;
        }
        esp_err_t err = s_state.ops.set_web_logs_enabled
            ? s_state.ops.set_web_logs_enabled(s_state.ops.user_ctx, enabled)
            : ESP_ERR_NOT_SUPPORTED;
        if (err == ESP_OK) {
            s_state.snapshot.web_logs_enabled = enabled;
        } else {
            ESP_LOGE(TAG, "set Web live logs failed: %s", esp_err_to_name(err));
            mosaic_top_notice_show(ui, &s_top_notice,
                TR("Debug", "调试"),
                TR("Unable to save log setting", "无法保存日志设置"),
                SETTINGS_NOTICE_DURATION_MS);
        }
        (void)settings_render_web_logs(ui);
        return;
    }
    case GSP_ACT_ID_WLAN_TOGGLE:
        if (event->timestamp_us - s_state.wlan_toggle_last_us < 100000) {
            return;
        }
        s_state.wlan_toggle_last_us = event->timestamp_us;
        const bool enabled = !s_state.wlan.enabled;
        if (mosaic_settings_set_wifi_enabled(enabled) != ESP_OK) {
            ESP_LOGW(TAG, "set WLAN enabled state failed");
            return;
        }
        /* Update presentation immediately; the provider remains
         * authoritative and reconciles this optimistic state later. */
        s_state.wlan.enabled = enabled;
        s_state.wlan.state = enabled
            ? MOSAIC_SETTINGS_WIFI_IDLE : MOSAIC_SETTINGS_WIFI_DISABLED;
        if (!enabled) {
            s_state.wlan_connect_pending = false;
            s_state.wlan_connect_deadline_us = 0;
            s_state.wlan_scan_waiting = false;
            s_state.wlan_scan_next_us = 0;
            s_state.wlan.connected = false;
            s_state.wlan.current_ssid[0] = '\0';
            settings_wlan_list_park(ui);
        } else {
            s_state.wlan_scan_next_us = event->timestamp_us;
            settings_wlan_request_scan();
            settings_wlan_list_attach(ui);
        }
        (void)settings_wlan_render(ui);
        return;
    case GSP_ACT_ID_WLAN_NETWORK_SELECT:
        settings_wlan_handle_network_select(ui, event);
        return;
    case GSP_ACT_ID_WLAN_CURRENT_NETWORK:
        settings_wlan_open_detail(ui);
        return;
    case GSP_ACT_ID_WLAN_CONNECT:
        if (s_state.wlan_password_active) {
            if (settings_wlan_handle_connect(ui)) {
                settings_wlan_pop_to_wlan(ui);
            }
        }
        return;
    case GSP_ACT_ID_SETTINGS_WLAN_PASSWORD_KEYBOARD_KEY:
        if (!s_state.wlan_password_active ||
                event->data.call.arg != 13U) {
            return;
        }
        if (settings_wlan_handle_connect(ui)) {
            settings_wlan_pop_to_wlan(ui);
        }
        return;
    case GSP_ACT_ID_WLAN_PASSWORD_DETACH:
        settings_wlan_cancel_password(ui);
        return;
    case GSP_ACT_ID_WLAN_PHONE_SETUP_OPEN:
        settings_wlan_password_detach(ui);
        /* The device may already be connected to another WLAN. Capture the
         * current transaction so that association cannot be mistaken for the
         * result of credentials submitted through this phone portal. */
        settings_wlan_refresh_status(ui);
        s_state.wlan_phone_active = true;
        s_state.wlan_phone_exit_pending = false;
        s_state.wlan_phone_operation_id =
            s_state.snapshot.network.operation_id;
        s_state.rendered_phone_qr[0] = '\0';
        settings_wlan_phone_refresh(ui);
        return;
    case GSP_ACT_ID_WLAN_PHONE_SETUP_CLOSE:
        s_state.wlan_phone_active = false;
        s_state.wlan_phone_exit_pending = false;
        s_state.wlan_phone_operation_id = 0;
        settings_wlan_password_attach(ui);
        return;
    case GSP_ACT_ID_WLAN_PHONE_SUBMITTED:
        settings_wlan_handle_phone_submitted(ui);
        return;
    case GSP_ACT_ID_WLAN_LIST_RESUME:
        if (!s_state.wlan_detail_active ||
                s_state.wlan_detail_leave_pending ||
                s_state.wlan_forget_pending) {
            return;
        }
        s_state.wlan_detail_leave_pending = true;
        if (esp_gsp_stack_view_pop(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, false) != ESP_GSP_OK) {
            s_state.wlan_detail_leave_pending = false;
        }
        return;
    case GSP_ACT_ID_WLAN_AUTO_JOIN_TOGGLE:
        s_state.wlan.auto_join = !s_state.wlan.auto_join;
        (void)settings_wlan_render(ui);
        return;
    case GSP_ACT_ID_WLAN_FORGET_NETWORK:
        settings_wlan_handle_forget(ui);
        return;
    default:
        return;
    }
}

static int32_t settings_level_from_x(int32_t x)
{
    if (x <= SETTINGS_TRACK_X_MIN) {
        return 0;
    }
    if (x >= SETTINGS_TRACK_X_MAX) {
        return 100;
    }
    const int32_t span = SETTINGS_TRACK_X_MAX - SETTINGS_TRACK_X_MIN;
    return ((x - SETTINGS_TRACK_X_MIN) * 100 + span / 2) / span;
}

static bool settings_display_page_is_active(esp_gsp_handle_t ui)
{
    uint16_t page = 0;
    return esp_gsp_stack_view_get_top(
               ui, GSP_OBJ_KEY_SETTINGS_STACK, &page) == ESP_GSP_OK &&
           (page == SETTINGS_DISPLAY_PAGE || page == SETTINGS_SOUND_PAGE);
}

static bool settings_factory_dispatch_pointer(
    esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    uint16_t page = 0;
    if (esp_gsp_stack_view_get_top(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &page) != ESP_GSP_OK ||
            page != SETTINGS_FACTORY_PAGE) {
        return false;
    }
    if (!event->data.pointer.pressed) {
        if (s_state.factory_hold_active) {
            s_state.factory_hold_active = false;
            (void)gsp_settings_settings_factory_hold_progress_set_value(
                ui, 0);
        }
        return true;
    }
    if (s_state.factory_hold_active || s_state.factory_hold_completed) {
        return true;
    }
    const int32_t x = event->data.pointer.x;
    const int32_t y = event->data.pointer.y;
    if (x >= SETTINGS_FACTORY_HOLD_X_MIN &&
            x <= SETTINGS_FACTORY_HOLD_X_MAX &&
            y >= SETTINGS_FACTORY_HOLD_Y_MIN &&
            y <= SETTINGS_FACTORY_HOLD_Y_MAX) {
        s_state.factory_hold_active = true;
        s_state.factory_hold_started_us = event->timestamp_us > 0
            ? event->timestamp_us : esp_timer_get_time();
    }
    return true;
}

static void settings_dispatch_pointer(
    esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    if (settings_factory_dispatch_pointer(ui, event)) {
        return;
    }
    if (!event->data.pointer.pressed) {
        if (s_state.brightness_drag_active && s_state.brightness_dirty &&
                s_state.brightness_hw_available &&
                s_state.ops.set_brightness != NULL) {
            esp_err_t err = s_state.ops.set_brightness(
                s_state.ops.user_ctx, s_state.brightness, true);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                s_state.brightness_hw_available = false;
            } else if (err != ESP_OK) {
                ESP_LOGE(TAG, "persist brightness failed: %s",
                         esp_err_to_name(err));
                settings_refresh_snapshot(ui);
            }
        }
        if (s_state.volume_drag_active && s_state.volume_dirty &&
                s_state.ops.set_volume != NULL) {
            esp_err_t err = s_state.ops.set_volume(
                s_state.ops.user_ctx, s_state.volume, true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "persist volume failed: %s",
                         esp_err_to_name(err));
                settings_refresh_snapshot(ui);
            }
        }
        if (s_state.display_pointer_pressed &&
                !s_state.brightness_drag_active &&
                !s_state.volume_drag_active) {
            if (!s_state.display_pointer_scrolled) {
                if (s_state.display_tap ==
                           SETTINGS_DISPLAY_TAP_NOTIFICATION) {
                    s_state.sound_effect_enabled =
                        !s_state.sound_effect_enabled;
                    settings_display_request_render();
                }
            }
        }
        s_state.brightness_drag_active = false;
        s_state.brightness_dirty = false;
        s_state.volume_drag_active = false;
        s_state.volume_dirty = false;
        s_state.display_pointer_pressed = false;
        s_state.display_pointer_scrolled = false;
        s_state.display_tap = SETTINGS_DISPLAY_TAP_NONE;
        return;
    }
    if (!s_state.brightness_drag_active && !s_state.volume_drag_active) {
        /* Only the initial press may claim a slider. A vertical scroll that
         * later crosses a track must stay a scroll gesture. */
        if (s_state.display_pointer_pressed) {
            const int32_t y = event->data.pointer.y;
            const int32_t distance = y - s_state.display_press_y;
            if (distance > SETTINGS_DISPLAY_TAP_SLOP ||
                    distance < -SETTINGS_DISPLAY_TAP_SLOP) {
                s_state.display_pointer_scrolled = true;
                s_state.display_tap = SETTINGS_DISPLAY_TAP_NONE;
            }
            s_state.display_last_y = y;
            return;
        }
        if (!settings_display_page_is_active(ui)) {
            return;
        }
        const int32_t y = event->data.pointer.y;
        if (y < SETTINGS_DISPLAY_VIEWPORT_Y ||
                y >= SETTINGS_DISPLAY_VIEWPORT_Y +
                     SETTINGS_DISPLAY_VIEWPORT_H) {
            return;
        }
        s_state.display_pointer_pressed = true;
        s_state.display_pointer_scrolled = false;
        s_state.display_press_y = y;
        s_state.display_last_y = y;
        s_state.display_tap = SETTINGS_DISPLAY_TAP_NONE;
        uint16_t active_page = 0;
        if (esp_gsp_stack_view_get_top(
                ui, GSP_OBJ_KEY_SETTINGS_STACK,
                &active_page) != ESP_GSP_OK) {
            return;
        }
        if (active_page == SETTINGS_DISPLAY_PAGE &&
                y >= SETTINGS_BRIGHTNESS_TRACK_Y_MIN &&
                y <= SETTINGS_BRIGHTNESS_TRACK_Y_MAX) {
            s_state.brightness_drag_active = true;
        } else if (active_page == SETTINGS_SOUND_PAGE &&
                   y >= SETTINGS_VOLUME_TRACK_Y_MIN &&
                   y <= SETTINGS_VOLUME_TRACK_Y_MAX) {
            s_state.volume_drag_active = true;
        } else if (active_page == SETTINGS_SOUND_PAGE &&
                   y >= SETTINGS_NOTIFICATION_Y_MIN &&
                   y <= SETTINGS_NOTIFICATION_Y_MAX) {
            s_state.display_tap = SETTINGS_DISPLAY_TAP_NOTIFICATION;
        }
    }
    if (!s_state.brightness_drag_active && !s_state.volume_drag_active) {
        return;
    }
    if (!s_state.brightness_drag_active && !s_state.volume_drag_active) {
        return;
    }
    const int32_t level = settings_level_from_x(event->data.pointer.x);
    if (s_state.brightness_drag_active) {
        if (level == s_state.brightness) {
            return;
        }
        if (s_state.brightness_hw_available &&
                s_state.ops.set_brightness != NULL) {
            esp_err_t err = s_state.ops.set_brightness(
                s_state.ops.user_ctx, level, false);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                s_state.brightness_hw_available = false;
                ESP_LOGW(TAG,
                         "panel brightness is unsupported; UI remains active");
            } else if (err != ESP_OK) {
                ESP_LOGE(TAG, "set brightness failed: %s",
                         esp_err_to_name(err));
                return;
            }
        }
        s_state.brightness = level;
        s_state.snapshot.brightness = level;
        s_state.brightness_dirty = true;
        settings_display_request_render();
        return;
    }
    const int32_t volume = level;
    if (volume == s_state.volume) {
        return;
    }
    esp_err_t err = ESP_OK;
    if (s_state.ops.set_volume != NULL) {
        err = s_state.ops.set_volume(
            s_state.ops.user_ctx, volume, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set volume failed: %s", esp_err_to_name(err));
            return;
        }
    }
    s_state.volume = volume;
    s_state.snapshot.volume = volume;
    s_state.volume_dirty = true;
    settings_display_request_render();
}

static void settings_event(
    esp_gsp_handle_t ui, const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case MOSAIC_EVENT_START:
        s_state.ui = ui;
        atomic_store_explicit(
            &s_state.display_render_pending, false, memory_order_release);
        s_state.display_render_timer = esp_gsp_timer_create(
            ui, SETTINGS_DISPLAY_RENDER_PERIOD_MS,
            settings_display_render_timer_cb, NULL);
        if (s_state.display_render_timer == NULL) {
            ESP_LOGE(TAG, "create Display render timer failed");
        }
        /* App state is static, while every open owns a new GSP handle and
         * List pool. Never carry navigation/list lifecycle across a forced
         * home-swipe close and subsequent reopen. */
        s_state.wlan_page_active = false;
        s_state.wlan_entry_pending = false;
        s_state.wlan_leave_pending = false;
        s_state.wlan_detail_active = false;
        s_state.wlan_detail_leave_pending = false;
        s_state.wlan_password_active = false;
        s_state.wlan_phone_active = false;
        s_state.wlan_phone_exit_pending = false;
        s_state.wlan_phone_operation_id = 0;
        s_state.wlan_forget_pending = false;
        s_state.wlan_connect_pending = false;
        s_state.wlan_connect_operation_id = 0;
        s_state.wlan_connect_deadline_us = 0;
        s_state.wlan_scan_revision = 0;
        s_state.wlan_scan_next_us = 0;
        s_state.wlan_scan_waiting = false;
        s_state.wlan_notice_pending = false;
        s_state.wlan_notice_title = NULL;
        s_state.wlan_notice_message = NULL;
        s_state.factory_hold_active = false;
        s_state.factory_hold_completed = false;
        s_state.factory_hold_started_us = 0;
        s_state.wlan.selected_ssid[0] = '\0';
        s_state.wlan.password[0] = '\0';
        s_root_list = ESP_GSP_LIST_NONE;
        settings_wlan_lists_reset();
        s_detail_list = ESP_GSP_LIST_NONE;
        s_city_province = -1;
        s_state.wlan_scan_compact_valid = false;
        s_wlan_network_count = 0;
        s_state.display_page_active = false;
        s_state.display_reset_pending = false;
        s_state.display_pointer_pressed = false;
        s_state.display_pointer_scrolled = false;
        s_state.display_tap = SETTINGS_DISPLAY_TAP_NONE;
        s_state.brightness_drag_active = false;
        s_state.brightness_dirty = false;
        s_state.brightness_hw_available = true;
        s_state.volume_drag_active = false;
        s_state.volume_dirty = false;
        s_state.integration_page = 0;
        s_state.integration_rendered_progress = UINT8_MAX;
        s_state.rendered_channel_qr[0] = '\0';
        s_state.rendered_phone_qr[0] = '\0';
        settings_refresh_snapshot(ui);
        s_state.wlan_scan_revision =
            s_state.snapshot.network.scan_revision;
        settings_root_list_attach(ui);
        if (s_state.snapshot.network.ssid[0] != '\0') {
            s_state.wlan.connected = s_state.snapshot.network.connected;
            strlcpy(s_state.wlan.current_ssid,
                    s_state.snapshot.network.ssid,
                    sizeof(s_state.wlan.current_ssid));
        }
        (void)settings_wlan_render(ui);
        break;
    case MOSAIC_EVENT_UI_CALL:
        settings_dispatch_call(ui, event);
        break;
    case MOSAIC_EVENT_POINTER:
        settings_dispatch_pointer(ui, event);
        break;
    case MOSAIC_EVENT_MODEL_CHANGED:
        settings_refresh_snapshot(ui);
        settings_wlan_refresh_status(ui);
        settings_root_list_refresh(ui);
        settings_refresh_about_detail(ui);
        uint16_t model_top = 0U;
        if (s_detail_kind == SETTINGS_DETAIL_BATTERY &&
                esp_gsp_stack_view_get_top(
                    ui, GSP_OBJ_KEY_SETTINGS_STACK,
                    &model_top) == ESP_GSP_OK &&
                model_top == SETTINGS_DETAIL_PAGE) {
            settings_refresh_battery_detail(ui);
        }
        if (settings_wlan_reconcile_navigation(ui)) {
            break;
        }
        if (s_state.wlan_page_active && !s_state.wlan_leave_pending &&
                !s_state.wlan_detail_active &&
                !s_state.wlan_password_active && s_state.wlan.enabled) {
            /* Scan start/end can emit several provider revisions while the
             * config store is busy. Keep this path lightweight: the WLAN page
             * needs the completed scan cache, not a full battery/config/QR
             * snapshot for every intermediate Wi-Fi state. */
            settings_wlan_scan(ui);
            settings_wlan_list_attach(ui);
        }
        break;
    case MOSAIC_EVENT_TIMER:
        /* Also consume the asynchronous cache on the 1 s App tick. This makes
         * list delivery independent of a transiently full loader command
         * queue. settings_wlan_scan refreshes only when content changed. */
        if (s_state.integration_page == SETTINGS_INTEGRATIONS_CHANNELS_PAGE) {
            settings_channels_refresh(ui);
        } else if (s_state.integration_page ==
                SETTINGS_INTEGRATIONS_MODEL_PAGE) {
            if (s_state.llm_phase == SETTINGS_LLM_STATUS) {
                settings_llm_refresh(ui);
            }
            if (s_state.llm_phase == SETTINGS_LLM_PROGRESS) {
                const int64_t elapsed = event->timestamp_us -
                    s_state.integration_started_us;
                uint8_t progress = elapsed <= 0 ? 0 :
                    (uint8_t)((elapsed * SETTINGS_INTEGRATION_PROGRESS_COUNT) /
                        SETTINGS_INTEGRATION_CONFIG_US);
                if (progress > SETTINGS_INTEGRATION_PROGRESS_COUNT) {
                    progress = SETTINGS_INTEGRATION_PROGRESS_COUNT;
                }
                if (progress != s_state.integration_rendered_progress) {
                    s_state.integration_rendered_progress = progress;
                    settings_integration_progress(
                        ui, s_integration_llm_progress, progress);
                }
                if (event->timestamp_us >= s_state.integration_deadline_us) {
                    settings_llm_refresh(ui);
                    if (!mosaic_settings_llm_is_configured(
                            &s_state.snapshot)) {
                        s_state.llm_phase = SETTINGS_LLM_CONFIGURING;
                        settings_llm_render(ui);
                        (void)mosaic_top_notice_show(
                            ui, &s_top_notice,
                            TR("Configuration not found", "未找到配置"),
                            TR("Submit the form on your phone and try again",
                               "请在手机上提交表单后重试"),
                            SETTINGS_NOTICE_DURATION_MS);
                    }
                }
            }
        }
        if (settings_wlan_reconcile_navigation(ui)) {
            break;
        }
        /* Give a pending notice this update transaction exclusively. This is
         * what makes the capsule reliable on the smaller ESP update queue. */
        if (settings_wlan_show_pending_notice(ui)) {
            break;
        }
        /* Device events normally invalidate the App immediately. This poll
         * also advances the deterministic host provider and covers a dropped
         * invalidation without changing the asynchronous device contract. */
        if (s_state.wlan_page_active || s_state.wlan_connect_pending ||
                s_state.wlan_phone_active) {
            settings_wlan_refresh_status(ui);
        }
        const int64_t wlan_now = esp_timer_get_time();
        if (s_state.wlan_connect_pending &&
                wlan_now >= s_state.wlan_connect_deadline_us) {
            s_state.wlan_connect_pending = false;
            s_state.wlan_notice_title = TR("Connection timed out", "连接超时");
            s_state.wlan_notice_message =
                TR("Check the network and try again", "请检查网络后重试");
            s_state.wlan_notice_pending = true;
            s_state.wlan_scan_next_us = wlan_now;
            (void)settings_wlan_render(ui);
        }
        uint16_t settings_top = 0U;
        if (s_detail_kind == SETTINGS_DETAIL_BATTERY &&
                esp_gsp_stack_view_get_top(
                    ui, GSP_OBJ_KEY_SETTINGS_STACK,
                    &settings_top) == ESP_GSP_OK &&
                settings_top == SETTINGS_DETAIL_PAGE) {
            settings_refresh_battery_detail(ui);
        }
        if (s_state.wlan_page_active && !s_state.wlan_leave_pending &&
                !s_state.wlan_detail_active &&
                !s_state.wlan_password_active &&
                !s_state.wlan_connect_pending &&
                !s_state.wlan_phone_active && s_state.wlan.enabled) {
            settings_wlan_scan(ui);
            settings_wlan_list_attach(ui);
            if (s_state.snapshot.network.radio_state ==
                    MOSAIC_SETTINGS_WIFI_RADIO_ON &&
                    wlan_now >= s_state.wlan_scan_next_us) {
                s_state.wlan_scan_waiting = false;
                settings_wlan_request_scan();
            }
        }
        break;
    case MOSAIC_EVENT_SCENE_CHANGED:
        (void)settings_wlan_reconcile_navigation(ui);
        break;
    case MOSAIC_EVENT_STOP:
        (void)esp_gsp_canvas_stop(
            ui, GSP_BIND_SETTINGS_WLAN_PHONE_QR_CANVAS);
        (void)esp_gsp_canvas_stop(
            ui, GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONFIG_QR_CANVAS);
        (void)esp_gsp_canvas_stop(
            ui, GSP_BIND_SETTINGS_CHANNELS_CONFIG_QR_CANVAS);
        const esp_gsp_err_t qr_flush = esp_gsp_flush(ui, 1000U);
        if (qr_flush != ESP_GSP_OK) {
            ESP_LOGW(TAG, "QR shutdown flush failed: %s",
                esp_err_to_name((esp_err_t)qr_flush));
        } else if (!settings_qr_free_buffers()) {
            ESP_LOGW(TAG, "QR buffers still busy after shutdown flush");
        }
        if (s_state.display_render_timer != NULL) {
            (void)esp_gsp_timer_delete(
                ui, s_state.display_render_timer);
            s_state.display_render_timer = NULL;
        }
        atomic_store_explicit(
            &s_state.display_render_pending, false, memory_order_release);
        mosaic_top_notice_detach(ui);
        (void)settings_wlan_set_loading(ui, false);
        settings_wlan_password_detach(ui);
        settings_wlan_list_park(ui);
        s_root_list = ESP_GSP_LIST_NONE;
        settings_wlan_lists_reset();
        s_detail_list = ESP_GSP_LIST_NONE;
        s_city_province = -1;
        s_state.wlan_scan_compact_valid = false;
        s_state.wlan_notice_pending = false;
        s_state.wlan_notice_title = NULL;
        s_state.wlan_notice_message = NULL;
        s_state.ui = NULL;
        s_state.wlan_page_active = false;
        s_state.wlan_entry_pending = false;
        s_state.wlan_leave_pending = false;
        s_state.wlan_detail_active = false;
        s_state.wlan_detail_leave_pending = false;
        s_state.wlan_password_active = false;
        s_state.wlan_phone_active = false;
        s_state.wlan_phone_exit_pending = false;
        s_state.wlan_phone_operation_id = 0;
        s_state.wlan_forget_pending = false;
        s_state.wlan_connect_pending = false;
        s_state.wlan_connect_operation_id = 0;
        s_state.wlan_connect_deadline_us = 0;
        s_state.wlan_scan_revision = 0;
        s_state.wlan_scan_next_us = 0;
        s_state.wlan_scan_waiting = false;
        s_state.display_page_active = false;
        s_state.display_pointer_pressed = false;
        s_state.brightness_drag_active = false;
        s_state.brightness_dirty = false;
        s_state.volume_drag_active = false;
        s_state.volume_dirty = false;
        s_state.integration_page = 0;
        break;
    default:
        break;
    }
}

static bool settings_back(esp_gsp_handle_t ui, int64_t timestamp_us)
{
    (void)timestamp_us;
    if (settings_detail_visible(ui, SETTINGS_DETAIL_CITY) &&
            s_city_province >= 0) {
        /* Leave the city list and return to the province list without popping
         * the detail page. */
        s_city_province = -1;
        settings_city_render_list(ui);
        (void)esp_gsp_list_scroll_to(ui, s_detail_list, 0);
        return true;
    }
    s_state.integration_page = 0;
    uint16_t page = 0;
    if (esp_gsp_stack_view_get_top(ui, GSP_OBJ_KEY_SETTINGS_STACK, &page) != ESP_GSP_OK || page == 0) {
        return false;
    }
    /* Handle child-page Back here to bypass the loader's animated pop. */
    esp_gsp_err_t err = esp_gsp_stack_view_pop(ui, GSP_OBJ_KEY_SETTINGS_STACK, false);
    if (err != ESP_GSP_OK) {
        ESP_LOGW(TAG, "return from Settings page failed: %d", (int)err);
    }
    return true;
}

const mosaic_app_descriptor_t mosaic_settings_app = {
    .id = 6,
    .launch_action = GSP_ACT_ID_APP_SETTINGS,
    /* Settings page navigation uses local action 0 for dynamic detail rows.
     * Keep App exit on the Shell-only sentinel so a row click cannot also be
     * interpreted by the runtime as Back-to-Hub. */
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .name = "settings",
    .title = "SETTINGS",
    .directory = &gsp_obj_directory_settings,
    .root_stack_key = GSP_OBJ_KEY_SETTINGS_STACK,
    .disable_swipe = true,
    .root_header_in_stack = false,
    /* Root, WLAN and Detail StackView pages retain their materialized
     * rows while another page is on top. Reserve the aggregate template
     * capacity so a child list cannot exhaust the shared pool. */
    .instance_slots =
        GSP_TEMPLATE_SETTINGS_ROOT_ROW_MAX_INSTANCES +
        GSP_TEMPLATE_SETTINGS_WLAN_ROW_MAX_INSTANCES +
        GSP_TEMPLATE_SETTINGS_WLAN_CONNECTED_ROW_MAX_INSTANCES +
        GSP_TEMPLATE_SETTINGS_DETAIL_ROW_MAX_INSTANCES,
    .dynamic_image_slots =
        GSP_TEMPLATE_SETTINGS_ROOT_ROW_MAX_INSTANCES +
        GSP_SETTINGS_DYNAMIC_IMAGE_SLOTS,
    .on_back = settings_back,
    .on_event = settings_event,
};
