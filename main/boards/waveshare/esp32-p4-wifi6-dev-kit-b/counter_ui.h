#pragma once
#include <lvgl.h>
#include <atomic>

class CounterUi {
public:
    void Show(lv_obj_t* parent);
    // Physical-console smoke test uses the same event as a touch, under LVGL lock.
    bool PressRecordingButton();
    void ShowAccountPage() { Render(7); }
    bool PressBindingButton() {
        if (!binding_button_)
            return false;
        lv_obj_send_event(binding_button_, LV_EVENT_CLICKED, nullptr);
        return true;
    }
    void SetDiagnosticFreeze(bool freeze) { diagnostic_freeze_ = freeze; }

private:
    lv_obj_t* root_ = nullptr;
    bool diagnostic_freeze_ = false;
    lv_obj_t* recording_button_ = nullptr;
    lv_obj_t* recording_label_ = nullptr;
    lv_obj_t* recording_status_ = nullptr;
    std::atomic<bool> recording_action_pending_{false};
    std::atomic<bool> recording_action_failed_{false};
    bool recording_action_stop_ = false;
    bool observed_recording_ = false;
    bool recording_completed_ = false;
    int64_t recording_started_us_ = 0;
    void RenderRecordingControls();
    void UpdateRecordingControls();
    static void RecordingAction(lv_event_t* event);
    int page_ = 0;
    lv_timer_t* settings_timer_ = nullptr;
    lv_obj_t* settings_status_ = nullptr;
    lv_obj_t* binding_button_ = nullptr;
    lv_obj_t* settings_hint_ = nullptr;
    lv_obj_t* ssid_ = nullptr;
    lv_obj_t* password_ = nullptr;
    lv_obj_t* networks_ = nullptr;
    lv_obj_t* keyboard_ = nullptr;
    unsigned scan_revision_ = 0;
    void RenderSettings(int page);
    void UpdateSettings();
    static void SettingsAction(lv_event_t* event);
    void Render(int page);
    static void OnAction(lv_event_t* event);
};
