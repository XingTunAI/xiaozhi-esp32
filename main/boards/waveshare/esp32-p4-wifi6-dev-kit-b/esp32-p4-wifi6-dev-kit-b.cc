#include "audio/codecs/dummy_audio_codec.h"
#include "audio/codecs/es8311_audio_codec.h"
#include "board_peripherals.h"
#include "config.h"
#include "counter_display.h"
#include "voice_lab_client.h"
#include "display/lcd_display.h"
#include "lcd_init_cmds.h"
#include "usb_capture_codec.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <esp_idf_version.h>
#include <esp_lcd_ili9881c.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch_gt911.h>
#include <esp_ldo_regulator.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_system.h>
#include <esp_wifi.h>

namespace {
constexpr char TAG[] = "DevKitBScreen";

class ScreenBacklight : public Backlight {
public:
    explicit ScreenBacklight(i2c_master_dev_handle_t control) : control_(control) {}

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
        const uint8_t command[] = {0x96, static_cast<uint8_t>(brightness * 255 / 100)};
        const auto err = i2c_master_transmit(control_, command, sizeof(command), 50);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Backlight write failed: %s", esp_err_to_name(err));
        }
    }

private:
    i2c_master_dev_handle_t control_;
};
}  // namespace

// Dedicated P4 identity, reusing the Voice Lab application and transport.
class WaveshareDevKitBScreen : public WifiBoard {
public:
    WaveshareDevKitBScreen() {
        ESP_LOGI(TAG, "Boot reset reason=%d", static_cast<int>(esp_reset_reason()));
        InitializeI2c();
        InitializeScreenControl();
        InitializeDisplay();
        backlight_ = new ScreenBacklight(screen_control_);
        backlight_->SetBrightness(80);
        InitializeTouch();
    }

    Display* GetDisplay() override { return display_; }
    Backlight* GetBacklight() override { return backlight_; }

    AudioCodec* GetAudioCodec() override {
        if (codec_ == nullptr) {
            if (i2c_master_probe(i2c_bus_, 0x18, 100) == ESP_OK) {
                codec_ = new Es8311AudioCodec(
                    i2c_bus_, I2C_NUM_1, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                    AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                    AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
                    ES8311_CODEC_DEFAULT_ADDR, true, true);
            } else {
                ESP_LOGW(TAG, "ES8311 absent; audio unavailable, keeping UI operational");
                codec_ = new DummyAudioCodec(24000, 24000);
            }
            codec_ = new UsbCaptureCodec(codec_);
        }
        return codec_;
    }

    void StartNetwork() override {
        StartBoardPeripherals([this](bool connected) {
            if (network_event_callback_) {
                network_event_callback_(
                    connected ? NetworkEvent::Connected : NetworkEvent::Disconnected, "");
            }
        });
    }

    bool IsNetworkConnected() const override { return IsBoardNetworkConnected(); }

    bool HandleConsoleCommand(const std::string& command) override {
        if (command == "p4 ui-freeze" || command == "p4 ui-resume") {
            static_cast<CounterDisplay*>(display_)->SetDiagnosticFreeze(command == "p4 ui-freeze");
            ESP_LOGI(TAG, "UI diagnostic freeze=%d", command == "p4 ui-freeze");
            return true;
        }
        if (command == "p4 tasks") {
            std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + 8);
            configRUN_TIME_COUNTER_TYPE total = 0;
            const auto count = uxTaskGetSystemState(tasks.data(), tasks.size(), &total);
            for (UBaseType_t i = 0; i < count; ++i) {
                ESP_LOGI(TAG, "Task %s priority=%u runtime=%llu stack=%u", tasks[i].pcTaskName,
                         static_cast<unsigned>(tasks[i].uxCurrentPriority),
                         static_cast<unsigned long long>(tasks[i].ulRunTimeCounter),
                         static_cast<unsigned>(tasks[i].usStackHighWaterMark));
            }
            return true;
        }
        if (command == "p4 account-page") {
            static_cast<CounterDisplay*>(display_)->ShowAccountPage();
            ESP_LOGI(TAG, "Account page opened");
            return true;
        }
        if (command == "p4 binding-button") {
            ESP_LOGI(TAG, "Binding button event: %s",
                     static_cast<CounterDisplay*>(display_)->PressBindingButton() ? "sent"
                                                                                  : "page closed");
            return true;
        }
        if (command == "p4 record-button") {
            ESP_LOGI(TAG, "Recording button event: %s",
                     static_cast<CounterDisplay*>(display_)->PressRecordingButton() ? "sent"
                                                                                    : "disabled");
            return true;
        }
        if (command == "p4 status") {
            ESP_LOGI(TAG, "Binding diagnostics: confirmed=%d active=%d",
                     VoiceLabClient::GetInstance().IsCustomerBound(),
                     VoiceLabClient::GetInstance().IsBindingActive());
            const auto state = GetBoardPeripheralStatus();
            ESP_LOGI(TAG, "Network diagnostics: reset_reason=%d eth_ip=%s wifi_ip=%s c6=%s",
                     static_cast<int>(esp_reset_reason()), state.ethernet_ip.c_str(),
                     state.wifi_ip.c_str(), state.c6_version.c_str());
            return true;
        }
        const std::string prefix = "p4 c6-update ";
        if (command.compare(0, prefix.size(), prefix) != 0)
            return false;
        ESP_LOGI(TAG, "C6 update request: %s",
                 RequestBoardC6Update(command.substr(prefix.size())) ? "queued" : "rejected");
        return true;
    }

    BoardNetworkStatus GetBoardNetworkStatus() const override {
        const auto status = GetBoardPeripheralStatus();
        if (status.ethernet_ip != "--")
            return {status.ethernet_ip, 0, 0};
        wifi_ap_record_t ap{};
        if (status.wifi_ip != "--" && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            return {status.wifi_ip, ap.rssi, ap.primary};
        return {};
    }

    const char* GetNetworkStateIcon() override { return ""; }
    void SetPowerSaveLevel(PowerSaveLevel level) override { (void)level; }

private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t screen_control_ = nullptr;
    esp_ldo_channel_handle_t phy_power_ = nullptr;
    esp_lcd_dsi_bus_handle_t dsi_bus_ = nullptr;
    LcdDisplay* display_ = nullptr;
    ScreenBacklight* backlight_ = nullptr;
    esp_lcd_panel_io_handle_t touch_io_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    AudioCodec* codec_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t config = {};
        config.i2c_port = I2C_NUM_1;
        config.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        config.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        config.clk_source = I2C_CLK_SRC_DEFAULT;
        config.glitch_ignore_cnt = 7;
        config.flags.enable_internal_pullup = true;
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, &i2c_bus_));
    }

    void InitializeScreenControl() {
        // 7-DSI-TOUCH-A uses its own I2C controller, not GPIO26/27 PWM/reset.
        ESP_ERROR_CHECK(i2c_master_probe(i2c_bus_, DISPLAY_CONTROL_I2C_ADDRESS, 100));
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = DISPLAY_CONTROL_I2C_ADDRESS;
        config.scl_speed_hz = 100000;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_, &config, &screen_control_));
        const uint8_t commands[][2] = {{0x95, 0x11}, {0x95, 0x17}, {0x96, 0x00}};
        for (const auto& command : commands) {
            ESP_ERROR_CHECK(i2c_master_transmit(screen_control_, command, sizeof(command), 50));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "Screen power/reset controller detected at 0x45");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    void InitializeDisplay() {
        esp_ldo_channel_config_t power = {};
        power.chan_id = DISPLAY_PHY_LDO_CHANNEL;
        power.voltage_mv = DISPLAY_PHY_LDO_MV;
        ESP_ERROR_CHECK(esp_ldo_acquire_channel(&power, &phy_power_));

        esp_lcd_dsi_bus_config_t bus = {};
        bus.bus_id = 0;
        bus.num_data_lanes = 2;
        bus.lane_bit_rate_mbps = DISPLAY_DSI_LANE_BITRATE_MBPS;
        ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus, &dsi_bus_));

        esp_lcd_dbi_io_config_t dbi = {};
        dbi.virtual_channel = 0;
        dbi.lcd_cmd_bits = 8;
        dbi.lcd_param_bits = 8;
        esp_lcd_panel_io_handle_t io = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus_, &dbi, &io));

        esp_lcd_dpi_panel_config_t dpi = {};
        dpi.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
        dpi.dpi_clock_freq_mhz = 80;
        dpi.in_color_format = LCD_COLOR_FMT_RGB565;
        dpi.out_color_format = LCD_COLOR_FMT_RGB565;
        dpi.num_fbs = 1;
        dpi.video_timing.h_size = DISPLAY_WIDTH;
        dpi.video_timing.v_size = DISPLAY_HEIGHT;
        dpi.video_timing.hsync_pulse_width = 50;
        dpi.video_timing.hsync_back_porch = 239;
        dpi.video_timing.hsync_front_porch = 33;
        dpi.video_timing.vsync_pulse_width = 30;
        dpi.video_timing.vsync_back_porch = 20;
        dpi.video_timing.vsync_front_porch = 2;
        // Use the synchronous CPU copy path while validating dynamic UI + UAC2.
        // DMA2D is optional; do not enable its asynchronous draw hook here.

        ili9881c_vendor_config_t vendor = {};
        vendor.init_cmds = lcd_init_cmds;
        vendor.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
        vendor.mipi_config.dsi_bus = dsi_bus_;
        vendor.mipi_config.dpi_config = &dpi;
        vendor.mipi_config.lane_num = 2;

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = &vendor;
        esp_lcd_panel_handle_t panel = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9881c(io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
        display_ = new CounterDisplay(io, panel);
        ESP_LOGI(TAG, "7-DSI-TOUCH-A initialized: 720x1280 RGB565");
    }
    void InitializeTouch() {
        uint8_t address = 0;
        for (uint8_t candidate : {0x5d, 0x14}) {
            if (i2c_master_probe(i2c_bus_, candidate, 100) == ESP_OK) {
                address = candidate;
                break;
            }
        }
        if (!address) {
            ESP_LOGW(TAG, "Touch not detected; keeping the display operational");
            return;
        }
        esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        io_config.dev_addr = address;
        io_config.scl_speed_hz = 400000;
        esp_err_t err = esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &touch_io_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Touch IO unavailable: %s", esp_err_to_name(err));
            return;
        }
        esp_lcd_touch_config_t config = {};
        config.x_max = DISPLAY_WIDTH;
        config.y_max = DISPLAY_HEIGHT;
        config.rst_gpio_num = GPIO_NUM_NC;
        config.int_gpio_num = GPIO_NUM_NC;
        err = esp_lcd_touch_new_i2c_gt911(touch_io_, &config, &touch_);
        if (err != ESP_OK) {
            esp_lcd_panel_io_del(touch_io_);
            touch_io_ = nullptr;
            ESP_LOGW(TAG, "Touch initialization unavailable: %s", esp_err_to_name(err));
            return;
        }
        lvgl_port_touch_cfg_t touch_config = {};
        touch_config.disp = lv_display_get_default();
        touch_config.handle = touch_;
        if (lvgl_port_add_touch(&touch_config) == nullptr) {
            ESP_LOGW(TAG, "Touch input registration failed");
        } else {
            ESP_LOGI(TAG, "GT911 touch input registered at 0x%02x", address);
        }
    }
};

DECLARE_BOARD(WaveshareDevKitBScreen);
