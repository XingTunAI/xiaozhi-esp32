#include "board_peripherals.h"
#include "counter_ui.h"
#include "voice_lab_client.h"

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_hosted_host_fw_ver.h>
#include <esp_mac.h>
#include <esp_psram.h>
#include <esp_system.h>
#include <cstring>
#include <string>

LV_FONT_DECLARE(counter_font_20);

namespace {
constexpr uint32_t BG = 0x101716, CARD = 0x192320, GOLD = 0xE4C58A, WHITE = 0xF2F0E8;

lv_obj_t* Label(lv_obj_t* parent, int x, int y, int width, const char* value,
                uint32_t color = WHITE) {
    auto* obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_label_set_text(obj, value);
    lv_obj_set_style_text_font(obj, &counter_font_20, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(obj, 10, 0);
    return obj;
}

lv_obj_t* Button(lv_obj_t* root, int x, int y, int w, const char* text, CounterUi* owner,
                 intptr_t action, lv_event_cb_t callback) {
    auto* obj = lv_button_create(root);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, 52);
    lv_obj_set_style_bg_color(obj, lv_color_hex(CARD), 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_user_data(obj, owner);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, reinterpret_cast<void*>(action));
    auto* label = Label(obj, 0, 0, w - 16, text, GOLD);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return obj;
}

void SetText(lv_obj_t* label, const std::string& text) {
    if (label && text != lv_label_get_text(label))
        lv_label_set_text(label, text.c_str());
}
}  // namespace

void CounterUi::RenderSettings(int page) {
    Label(root_, 36, 27, 700, "设备设置", GOLD);
    Button(root_, 1050, 22, 190, "返回首页", this, 0, SettingsAction);
    Button(root_, 32, 118, 210, "网络配置", this, 4, SettingsAction);
    Button(root_, 32, 190, 210, "版本信息", this, 5, SettingsAction);
    Button(root_, 32, 262, 210, "存储与音频", this, 6, SettingsAction);
    Label(root_, 36, 656, 1170, "本机设置 · 网络配置保存到设备；演示计价与录音状态保持不变",
          0x94A49B);
    if (page == 4) {
        settings_status_ = Label(root_, 284, 104, 948, "正在读取网络状态");
        Label(root_, 284, 258, 360, "Wi-Fi 名称（可手动输入）", GOLD);
        Label(root_, 674, 258, 320, "密码（开放网络留空）", GOLD);
        ssid_ = lv_textarea_create(root_);
        password_ = lv_textarea_create(root_);
        for (auto* field : {ssid_, password_}) {
            lv_textarea_set_one_line(field, true);
            lv_obj_set_style_text_font(field, &counter_font_20, 0);
            lv_obj_set_style_text_color(field, lv_color_hex(WHITE), 0);
            lv_obj_set_style_bg_color(field, lv_color_hex(CARD), 0);
            lv_obj_set_size(field, field == ssid_ ? 360 : 320, 52);
            lv_obj_set_pos(field, field == ssid_ ? 284 : 674, 294);
            lv_obj_set_user_data(field, this);
            lv_obj_add_event_cb(
                field,
                [](lv_event_t* event) {
                    if (lv_event_get_code(event) != LV_EVENT_FOCUSED &&
                        lv_event_get_code(event) != LV_EVENT_CLICKED)
                        return;
                    auto* field = lv_event_get_current_target_obj(event);
                    auto* self = static_cast<CounterUi*>(lv_obj_get_user_data(field));
                    if (!self->keyboard_)
                        return;
                    lv_keyboard_set_textarea(self->keyboard_, field);
                    lv_obj_remove_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(self->keyboard_);
                },
                LV_EVENT_ALL, nullptr);
        }
        lv_textarea_set_max_length(ssid_, 32);
        lv_textarea_set_max_length(password_, 64);
        lv_textarea_set_password_mode(password_, true);
        Button(root_, 1022, 294, 210, "连接并保存", this, 101, SettingsAction);
        networks_ = lv_dropdown_create(root_);
        lv_obj_set_pos(networks_, 284, 374);
        lv_obj_set_size(networks_, 710, 52);
        lv_obj_set_style_text_font(networks_, &counter_font_20, 0);
        lv_obj_set_style_text_color(networks_, lv_color_hex(WHITE), 0);
        lv_obj_set_style_bg_color(networks_, lv_color_hex(CARD), 0);
        lv_obj_set_style_text_font(lv_dropdown_get_list(networks_), &counter_font_20, 0);
        lv_dropdown_set_options(networks_, "选择附近的 Wi-Fi");
        lv_obj_set_user_data(networks_, this);
        lv_obj_add_event_cb(
            networks_,
            [](lv_event_t* event) {
                auto* self =
                    static_cast<CounterUi*>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
                if (lv_dropdown_get_selected(self->networks_) == 0)
                    return;
                char selected[129];
                lv_dropdown_get_selected_str(self->networks_, selected, sizeof(selected));
                lv_textarea_set_text(self->ssid_, selected);
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        Button(root_, 1022, 374, 210, "扫描网络", this, 100, SettingsAction);
        settings_hint_ = Label(root_, 284, 464, 940,
                               "有线网络优先，自动获取地址。Wi-Fi 连接成功后保存配置。", 0x94A49B);
        keyboard_ = lv_keyboard_create(root_);
        lv_obj_set_size(keyboard_, 960, 252);
        // Keyboard constructors use BOTTOM_MID; absolute page coordinates must
        // explicitly override that alignment or the keyboard sits off-screen.
        lv_obj_align(keyboard_, LV_ALIGN_TOP_LEFT, 280, 390);
        lv_keyboard_set_mode(keyboard_, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(
            keyboard_,
            [](lv_event_t* event) {
                auto* kb = lv_event_get_target_obj(event);
                if (lv_event_get_code(event) == LV_EVENT_READY ||
                    lv_event_get_code(event) == LV_EVENT_CANCEL) {
                    lv_keyboard_set_textarea(kb, nullptr);
                    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
                }
            },
            LV_EVENT_ALL, nullptr);
        scan_revision_ = static_cast<unsigned>(-1);
    } else if (page == 5) {
        settings_status_ = Label(root_, 284, 104, 948, "正在读取版本信息");
    } else {
        settings_status_ = Label(root_, 284, 104, 948, "正在读取外设状态");
        Button(root_, 284, 470, 300, "检测 SD 卡", this, 102, SettingsAction);
        settings_hint_ = Label(
            root_, 284, 548, 940,
            "无卡可以正常使用。检测不会格式化或创建文件。\nUSB 音频外设接 USB-A，拨动开关置 HOST。",
            0x94A49B);
    }
    if (!settings_timer_) {
        settings_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<CounterUi*>(lv_timer_get_user_data(timer))->UpdateSettings();
            },
            1000, this);
    }
    UpdateSettings();
}

void CounterUi::UpdateSettings() {
    if (page_ < 4 || !settings_status_)
        return;
    const auto state = GetBoardPeripheralStatus();
    if (page_ == 4) {
        SetText(settings_status_, "有线网络：" + state.ethernet + "    IP：" + state.ethernet_ip +
                                      "\n网关：" + state.gateway +
                                      "    地址方式：自动获取\nWi-Fi：" + state.wifi + "    IP：" +
                                      state.wifi_ip);
        if (scan_revision_ != state.scan_revision && !lv_dropdown_is_open(networks_)) {
            std::string options = "选择附近的 Wi-Fi";
            for (const auto& ssid : state.networks)
                options += "\n" + ssid;
            lv_dropdown_set_options(networks_, options.c_str());
            scan_revision_ = state.scan_revision;
        }
    } else if (page_ == 5) {
        const auto* app = esp_app_get_description();
        esp_chip_info_t chip{};
        esp_chip_info(&chip);
        uint32_t flash_size = 0;
        esp_flash_get_size(nullptr, &flash_size);
        uint8_t mac[6]{};
        esp_read_mac(mac, ESP_MAC_ETH);
        char details[1200];
        snprintf(details, sizeof(details),
                 "设备：ESP32-P4 KIT-B / Rev1.2\nP4 芯片版本：%u.%u    内核：%u\n"
                 "应用版本：%s    配置：P4 屏幕与外设\n构建时间：%s %s\n"
                 "ESP-IDF：%s    Hosted 主机：%u.%u.%u\nC6 实际固件版本：%s\n"
                 "Flash：%lu MB    PSRAM：%u MB\n有线 MAC：%02X:%02X:%02X:%02X:%02X:%02X\n"
                 "固件标识：%02x%02x%02x%02x%02x%02x%02x%02x",
                 chip.revision / 100, chip.revision % 100, chip.cores, app->version, app->date,
                 app->time, esp_get_idf_version(), ESP_HOSTED_VERSION_MAJOR_1,
                 ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1, state.c6_version.c_str(),
                 static_cast<unsigned long>(flash_size / (1024 * 1024)),
                 static_cast<unsigned>(esp_psram_get_size() / (1024 * 1024)), mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5], app->app_elf_sha256[0], app->app_elf_sha256[1],
                 app->app_elf_sha256[2], app->app_elf_sha256[3], app->app_elf_sha256[4],
                 app->app_elf_sha256[5], app->app_elf_sha256[6], app->app_elf_sha256[7]);
        SetText(settings_status_, details);
    } else {
        SetText(settings_status_,
                "SD 卡：" + state.storage + "\n\nUSB 主机：" + state.usb +
                    "\n已识别 USB 音频接口：" + std::to_string(state.audio_interfaces) +
                    "\n\nVoice Lab: " +
                    (VoiceLabClient::GetInstance().IsConnected() ? "online" : "offline") +
                    "\nUSB capture: " +
                    (VoiceLabClient::GetInstance().IsRecording() ? "recording" : "idle") +
                    "\nPlayback: ES8311");
    }
}

void CounterUi::SettingsAction(lv_event_t* event) {
    auto* self = static_cast<CounterUi*>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    const auto action = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
    if (action < 100) {
        self->Render(action);
        return;
    }
    bool accepted = false;
    if (action == 100)
        accepted = RequestBoardWifiScan();
    else if (action == 101) {
        accepted = RequestBoardWifiConnect(lv_textarea_get_text(self->ssid_),
                                           lv_textarea_get_text(self->password_));
        if (accepted)
            lv_textarea_set_text(self->password_, "");
    } else if (action == 102)
        accepted = RequestBoardStorageCheck();
    SetText(self->settings_hint_, accepted ? "请求已提交，请等待状态更新。"
                                           : "暂时无法执行，请检查输入或等待当前操作完成。");
    if (self->keyboard_)
        lv_obj_add_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}
