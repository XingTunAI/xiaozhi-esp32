#pragma once
#include "counter_ui.h"
#include "display/lcd_display.h"

class CounterDisplay : public MipiLcdDisplay {
public:
    CounterDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
        : MipiLcdDisplay(io, panel, 720, 1280, 0, 0, false, false, false, true) {}
    void SetupUI() override {
        if (IsSetupUICalled())
            return;
        DisplayLockGuard guard(this);
        lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_270);
        width_ = 1280;
        height_ = 720;
        ui_.Show(lv_display_get_screen_active(display_));
        Display::SetupUI();
    }
    // This UI owns its styles and does not create the base chat/status widgets.
    // Asset loading can replace fonts and reapply a theme after networking starts.
    void SetTheme(Theme* theme) override { Display::SetTheme(theme); }
    // Keep assistant chat messages outside the counter interface.
    void SetStatus(const char*) override {}
    void SetChatMessage(const char*, const char*) override {}
    void ClearChatMessages() override {}
    void SetEmotion(const char*) override {}
    void UpdateStatusBar(bool = false) override {}
    void ShowNotification(const char*, int = 3000) override {}
    void ShowNotification(const std::string&, int = 3000) override {}
    void SetPowerSaveMode(bool) override {}
    bool PressRecordingButton() {
        DisplayLockGuard guard(this);
        return ui_.PressRecordingButton();
    }
    void SetDiagnosticFreeze(bool freeze) {
        DisplayLockGuard guard(this);
        ui_.SetDiagnosticFreeze(freeze);
    }

private:
    CounterUi ui_;
};
