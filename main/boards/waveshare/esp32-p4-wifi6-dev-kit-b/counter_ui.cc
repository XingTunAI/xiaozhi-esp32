#include "counter_ui.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "counter_ui_layout.h"
#include "voice_lab_client.h"

namespace {
lv_obj_t* CreateBox(lv_obj_t* parent, const CounterUiItem& item) {
    auto obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, item.x, item.y);
    lv_obj_set_size(obj, item.w, item.h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(item.color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, item.radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}
}  // namespace

void CounterUi::Show(lv_obj_t* parent) {
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, 1280, 720);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101716), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    Render(0);
    lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<CounterUi*>(lv_timer_get_user_data(timer))->UpdateRecordingControls();
        },
        250, this);
}

void CounterUi::Render(int page) {
    page_ = page;
    recording_button_ = recording_label_ = recording_status_ = nullptr;
    settings_status_ = settings_hint_ = ssid_ = password_ = networks_ = keyboard_ = nullptr;
    lv_obj_clean(root_);
    if (page >= 4) {
        RenderSettings(page);
        RenderRecordingControls();
        return;
    }
    const auto& layout = kCounterPages[page];
    for (size_t i = 0; i < layout.count; ++i) {
        const auto& item = layout.items[i];
        if (item.text == nullptr) {
            CreateBox(root_, item);
            continue;
        }
        lv_obj_t* parent = root_;
        if (item.action >= 0) {
            parent = CreateBox(root_, item);
            lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(parent, this);
            lv_obj_add_event_cb(parent, OnAction, LV_EVENT_CLICKED,
                                const_cast<CounterUiItem*>(&item));
            lv_obj_set_style_bg_opa(parent, LV_OPA_80, LV_STATE_PRESSED);
        }
        auto label = lv_label_create(parent);
        lv_label_set_text(label, item.text);
        lv_obj_set_style_text_font(label, item.font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(item.ink), 0);
        if (item.action >= 0) {
            lv_obj_center(label);
        } else {
            lv_obj_set_pos(label, item.x, item.y);
            lv_obj_set_width(label, item.w);
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_align(label, item.align, 0);
        }
    }
    RenderRecordingControls();
}

void CounterUi::RenderRecordingControls() {
    auto* bar = lv_obj_create(root_);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 32, 656);
    lv_obj_set_size(bar, 1216, 64);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x101716), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    recording_status_ = lv_label_create(bar);
    lv_obj_set_pos(recording_status_, 12, 20);
    lv_obj_set_width(recording_status_, 900);
    lv_obj_set_style_text_font(recording_status_, &counter_font_20, 0);
    lv_obj_set_style_text_color(recording_status_, lv_color_hex(0xF2F0E8), 0);
    recording_button_ = lv_button_create(bar);
    lv_obj_set_pos(recording_button_, 966, 6);
    lv_obj_set_size(recording_button_, 238, 52);
    lv_obj_set_style_bg_color(recording_button_, lv_color_hex(0xE4C58A), 0);
    lv_obj_set_style_bg_opa(recording_button_, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(recording_button_, RecordingAction, LV_EVENT_CLICKED, this);
    recording_label_ = lv_label_create(recording_button_);
    lv_obj_set_style_text_font(recording_label_, &counter_font_20, 0);
    lv_obj_set_style_text_color(recording_label_, lv_color_hex(0x19221B), 0);
    UpdateRecordingControls();
}

void CounterUi::RecordingAction(lv_event_t* event) {
    auto* self = static_cast<CounterUi*>(lv_event_get_user_data(event));
    if (self->recording_action_pending_.exchange(true))
        return;
    self->recording_action_failed_.store(false);
    self->recording_action_stop_ = VoiceLabClient::GetInstance().IsRecording();
    // Neither a network send nor audio finalization may block the LVGL task.
    if (xTaskCreate(
            [](void* arg) {
                auto* ui = static_cast<CounterUi*>(arg);
                auto& client = VoiceLabClient::GetInstance();
                const bool ok = ui->recording_action_stop_ ? client.StopRecording()
                                                           : client.RequestStartRecording();
                ui->recording_action_failed_.store(!ok);
                ui->recording_action_pending_.store(false);
                vTaskDelete(nullptr);
            },
            "p4_record_ui", 8192, self, 2, nullptr) != pdPASS) {
        self->recording_action_failed_.store(true);
        self->recording_action_pending_.store(false);
    }
    self->UpdateRecordingControls();
}

bool CounterUi::PressRecordingButton() {
    if (!recording_button_ || lv_obj_has_state(recording_button_, LV_STATE_DISABLED))
        return false;
    lv_obj_send_event(recording_button_, LV_EVENT_CLICKED, nullptr);
    return true;
}

void CounterUi::UpdateRecordingControls() {
    if (diagnostic_freeze_)
        return;
    auto& client = VoiceLabClient::GetInstance();
    using State = VoiceLabClient::UserState;
    const auto state = client.GetUserState();
    const bool recording = client.IsRecording();
    if (recording && !observed_recording_) {
        recording_started_us_ = esp_timer_get_time();
        recording_completed_ = false;
        recording_action_failed_.store(false);
    }
    if (observed_recording_ && !recording)
        recording_completed_ = true;
    observed_recording_ = recording;
    if (!recording_button_)
        return;
    const char* action = "开始录音";
    const char* hint = recording_completed_ ? "录音已完成" : "服务已连接 · 等待录音";
    bool enabled = true;
    char elapsed[96];
    if (recording) {
        const auto seconds = (esp_timer_get_time() - recording_started_us_) / 1000000;
        snprintf(elapsed, sizeof(elapsed), "正在录音  %02lld:%02lld%s",
                 static_cast<long long>(seconds / 60), static_cast<long long>(seconds % 60),
                 client.IsConnected() ? "" : " · 服务连接中");
        action = "停止录音";
        hint = elapsed;
    } else if (state == State::Stopping) {
        action = "正在保存";
        hint = "正在确认剩余音频，请稍候";
        enabled = false;
    } else if (!client.IsConnected()) {
        hint = "服务未连接，请检查网络和设备配对";
        enabled = false;
    } else if (state == State::Requesting || state == State::Starting) {
        action = "正在申请";
        hint = "等待服务端授权录音";
        enabled = false;
    } else if (state == State::Error || recording_action_failed_.load()) {
        hint = "录音未完成，请检查服务端授权或网络";
    }
    if (recording_action_pending_.load())
        enabled = false;
    if (enabled)
        lv_obj_remove_state(recording_button_, LV_STATE_DISABLED);
    else
        lv_obj_add_state(recording_button_, LV_STATE_DISABLED);
    if (strcmp(lv_label_get_text(recording_label_), action) != 0) {
        lv_label_set_text(recording_label_, action);
        lv_obj_center(recording_label_);
    }
    if (strcmp(lv_label_get_text(recording_status_), hint) != 0)
        lv_label_set_text(recording_status_, hint);
}

void CounterUi::OnAction(lv_event_t* event) {
    auto target = lv_event_get_target_obj(event);
    auto self = static_cast<CounterUi*>(lv_obj_get_user_data(target));
    auto item = static_cast<const CounterUiItem*>(lv_event_get_user_data(event));
    const int page = item->action;
    self->Render(page);
}
