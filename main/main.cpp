#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "SdUsbManager.hpp"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "display/lv_display_private.h"
#include "esp_rom_sys.h"
#include <unistd.h>
#include "freertos/ringbuf.h"
#include "bsp_board_extra.h" 

static const char *TAG = "DoomFW";
#define BOOT_BTN_PIN GPIO_NUM_0

extern "C" {
    void doomgeneric_Create(int argc, char **argv);
    void doomgeneric_Tick();
}

#define DOOM_WIDTH 320
#define CHUNK_LINES 20
static uint16_t* dma_buffer[2] = {nullptr, nullptr};
static uint8_t current_buf = 0;

static volatile bool emu_stop_requested = false;
static RingbufHandle_t audio_ringbuf = NULL;

static void audio_drain_task(void *arg) {
    size_t bytes_written;
    while (!emu_stop_requested) {
        size_t item_size;
        uint8_t *data = (uint8_t *)xRingbufferReceive(audio_ringbuf, &item_size, pdMS_TO_TICKS(100));
        if (data) {
            bsp_extra_i2s_write(data, item_size, &bytes_written, portMAX_DELAY);
            vRingbufferReturnItem(audio_ringbuf, (void *)data);
        }
    }
    vTaskDelete(NULL);
}

extern "C" void doom_push_audio_samples(int16_t *samples, size_t num_samples) {
    if (audio_ringbuf) {
        if (xRingbufferSend(audio_ringbuf, samples, num_samples * sizeof(int16_t), pdMS_TO_TICKS(10)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(1)); 
        }
    }
}

#define KEY_QUEUE_SIZE 32
struct KeyEvent { int pressed; unsigned char key; };
static KeyEvent key_queue[KEY_QUEUE_SIZE];
static int key_head = 0;
static int key_tail = 0;

static void push_key(int pressed, unsigned char key) {
    int next = (key_head + 1) % KEY_QUEUE_SIZE;
    if (next != key_tail) {
        key_queue[key_head].pressed = pressed;
        key_queue[key_head].key = key;
        key_head = next;
    }
}

extern "C" {
    #include "doomgeneric.h"
    void DG_Init() {}
    void DG_SleepMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
    uint32_t DG_GetTicksMs() { return esp_timer_get_time() / 1000; }
    
    int DG_GetKey(int* pressed, unsigned char* doomKey) { 
        if (key_head == key_tail) return 0; 
        *pressed = key_queue[key_tail].pressed;
        *doomKey = key_queue[key_tail].key;
        key_tail = (key_tail + 1) % KEY_QUEUE_SIZE;
        return 1; 
    }
    
    void DG_SetWindowTitle(const char * title) {}
    
    void DG_DrawFrame() {
        extern uint32_t* DG_ScreenBuffer;
        for (int chunk = 0; chunk < (200 / CHUNK_LINES); chunk++) {
            int src_y = chunk * CHUNK_LINES;
            uint16_t *dst = dma_buffer[current_buf];
            for (int y = 0; y < CHUNK_LINES; y++) {
                uint32_t *src_row = &DG_ScreenBuffer[(src_y + y) * DOOM_WIDTH];
                for (int x = 0; x < DOOM_WIDTH; x++) {
                    uint32_t color32 = src_row[x];
                    uint8_t r = (color32 >> 16) & 0xFF;
                    uint8_t g = (color32 >> 8) & 0xFF;
                    uint8_t b = color32 & 0xFF;
                    *dst++ = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                }
            }
            lv_area_t area;
            area.x1 = 44; area.y1 = 20 + (chunk * CHUNK_LINES);
            area.x2 = area.x1 + DOOM_WIDTH - 1; area.y2 = area.y1 + CHUNK_LINES - 1;
            
            if (lvgl_port_lock(pdMS_TO_TICKS(10))) {
                lv_display_t * disp = lv_display_get_default();
                if (disp && disp->flush_cb) disp->flush_cb(disp, &area, (uint8_t*)dma_buffer[current_buf]);
                lvgl_port_unlock();
            }
            current_buf = !current_buf;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

static void clear_i2c_bus(void) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_14) | (1ULL << GPIO_NUM_15);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    gpio_set_level(GPIO_NUM_15, 1); esp_rom_delay_us(10);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(GPIO_NUM_14, 0); esp_rom_delay_us(10);
        gpio_set_level(GPIO_NUM_14, 1); esp_rom_delay_us(10);
    }
    gpio_set_level(GPIO_NUM_15, 0); esp_rom_delay_us(10);
    gpio_set_level(GPIO_NUM_14, 1); esp_rom_delay_us(10);
    gpio_set_level(GPIO_NUM_15, 1); esp_rom_delay_us(10);

    gpio_reset_pin(GPIO_NUM_14);
    gpio_reset_pin(GPIO_NUM_15);
}

static void create_virtual_btn(lv_obj_t* parent, int x, int y, int w, int h, const char* symbol, unsigned char key_code, lv_color_t color) {
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, 0); 
    lv_obj_set_style_radius(btn, 15, 0);
    
    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, [](lv_event_t * e) {
        unsigned char k = (unsigned char)(uintptr_t)lv_event_get_user_data(e);
        push_key(1, k);
    }, LV_EVENT_PRESSED, (void*)(uintptr_t)key_code);

    lv_obj_add_event_cb(btn, [](lv_event_t * e) {
        unsigned char k = (unsigned char)(uintptr_t)lv_event_get_user_data(e);
        push_key(0, k);
    }, LV_EVENT_RELEASED, (void*)(uintptr_t)key_code);
    
    lv_obj_add_event_cb(btn, [](lv_event_t * e) {
        unsigned char k = (unsigned char)(uintptr_t)lv_event_get_user_data(e);
        push_key(0, k);
    }, LV_EVENT_PRESS_LOST, (void*)(uintptr_t)key_code);
}

extern "C" void app_main(void) {
    clear_i2c_bus();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BOOT_BTN_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    bsp_display_start();
    vTaskDelay(pdMS_TO_TICKS(100)); 

    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        lv_obj_t * scr = lv_scr_act(); 
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        
        #define KEY_RIGHTARROW 0xae
        #define KEY_LEFTARROW  0xac
        #define KEY_UPARROW    0xad
        #define KEY_DOWNARROW  0xaf
        #define KEY_RCTRL      (0x80+0x1d) 
        
        static int current_weapon = 2;

        create_virtual_btn(scr, 150, 300, 50, 50, "<", 0, lv_color_hex(0x888888));
        lv_obj_t * btn_prev = lv_obj_get_child(scr, -1);
        lv_obj_add_event_cb(btn_prev, [](lv_event_t * e) {
            push_key(1, '0' + current_weapon);
            current_weapon = (current_weapon <= 1) ? 7 : current_weapon - 1;
            push_key(1, '0' + current_weapon);
        }, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(btn_prev, [](lv_event_t * e) {
            push_key(0, '0' + current_weapon);
        }, LV_EVENT_RELEASED, NULL);

        create_virtual_btn(scr, 210, 300, 50, 50, ">", 0, lv_color_hex(0x888888));
        lv_obj_t * btn_next = lv_obj_get_child(scr, -1);
        lv_obj_add_event_cb(btn_next, [](lv_event_t * e) {
            push_key(1, '0' + current_weapon);
            current_weapon = (current_weapon >= 7) ? 1 : current_weapon + 1;
            push_key(1, '0' + current_weapon); 
        }, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(btn_next, [](lv_event_t * e) {
            push_key(0, '0' + current_weapon);
        }, LV_EVENT_RELEASED, NULL);

        create_virtual_btn(scr, 54, 290, 60, 60, LV_SYMBOL_UP,    KEY_UPARROW,    lv_color_hex(0x555555));
        create_virtual_btn(scr, 54, 430, 60, 60, LV_SYMBOL_DOWN,  KEY_DOWNARROW,  lv_color_hex(0x555555));
        create_virtual_btn(scr, 14, 360, 60, 60, LV_SYMBOL_LEFT,  KEY_LEFTARROW,  lv_color_hex(0x555555));
        create_virtual_btn(scr, 94, 360, 60, 60, LV_SYMBOL_RIGHT, KEY_RIGHTARROW, lv_color_hex(0x555555));

        create_virtual_btn(scr, 294, 290, 60, 60, "USE",   ' ',       lv_color_hex(0x33FF33)); 
        create_virtual_btn(scr, 294, 430, 60, 60, "FIRE",  KEY_RCTRL, lv_color_hex(0xFF3333)); 
        create_virtual_btn(scr, 234, 360, 60, 60, "ENT",   13,        lv_color_hex(0x3333FF)); 
        create_virtual_btn(scr, 334, 360, 60, 60, "ESC",   27,        lv_color_hex(0xAAAAAA));

        // ==========================================
        // CONTROLE DE VOLUME (Canto Superior Direito)
        // ==========================================
        
        // Botão de Aumentar Volume (+)
        lv_obj_t * btn_vol_up = lv_btn_create(scr);
        lv_obj_set_size(btn_vol_up, 40, 40);
        lv_obj_align(btn_vol_up, LV_ALIGN_TOP_RIGHT, -10, 50); // Fica na margem preta superior
        lv_obj_set_style_bg_color(btn_vol_up, lv_color_hex(0x444444), 0);
        lv_obj_set_style_radius(btn_vol_up, 10, 0);
        
        lv_obj_t * lbl_vup = lv_label_create(btn_vol_up);
        lv_label_set_text(lbl_vup, LV_SYMBOL_VOLUME_MAX);
        lv_obj_center(lbl_vup);
        
        lv_obj_add_event_cb(btn_vol_up, [](lv_event_t * e) {
            int vol = bsp_extra_codec_volume_get();
            if (vol < 100) vol += 10;
            bsp_extra_codec_volume_set(vol, NULL);
        }, LV_EVENT_CLICKED, NULL);

        // Botão de Diminuir Volume (-)
        lv_obj_t * btn_vol_down = lv_btn_create(scr);
        lv_obj_set_size(btn_vol_down, 40, 40);
        lv_obj_align(btn_vol_down, LV_ALIGN_TOP_RIGHT, -10, 110); // Fica logo abaixo do botão de aumentar
        lv_obj_set_style_bg_color(btn_vol_down, lv_color_hex(0x444444), 0);
        lv_obj_set_style_radius(btn_vol_down, 10, 0);
        
        lv_obj_t * lbl_vdn = lv_label_create(btn_vol_down);
        lv_label_set_text(lbl_vdn, LV_SYMBOL_VOLUME_MID);
        lv_obj_center(lbl_vdn);
        
        lv_obj_add_event_cb(btn_vol_down, [](lv_event_t * e) {
            int vol = bsp_extra_codec_volume_get();
            if (vol > 0) vol -= 10;
            bsp_extra_codec_volume_set(vol, NULL);
        }, LV_EVENT_CLICKED, NULL);

        bsp_display_unlock();
    }
    bsp_display_brightness_set(80);

    bsp_extra_codec_init();
    bsp_extra_codec_set_fs(44100, 16, I2S_SLOT_MODE_STEREO);
    bsp_extra_codec_mute_set(false);
    bsp_extra_codec_volume_set(80, NULL);
    
    audio_ringbuf = xRingbufferCreate(16384, RINGBUF_TYPE_BYTEBUF);
    xTaskCreatePinnedToCore(audio_drain_task, "audio_drain", 4096, NULL, 5, NULL, 0);

    SdUsbManager::get_instance().init_local_storage();

    FILE* f = fopen("/sdcard/DOOM/DOOM1.WAD", "r");
    if (!f) {
        ESP_LOGE(TAG, "ERRO CRITICO: /sdcard/DOOM/DOOM1.WAD não encontrado!");
        vTaskDelay(pdMS_TO_TICKS(3000)); 
        const esp_partition_t *factory_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (factory_part) {
            esp_ota_set_boot_partition(factory_part);
            esp_restart();
        }
    }
    fclose(f); 

    dma_buffer[0] = (uint16_t*)heap_caps_malloc(DOOM_WIDTH * CHUNK_LINES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    dma_buffer[1] = (uint16_t*)heap_caps_malloc(DOOM_WIDTH * CHUNK_LINES * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    chdir("/sdcard/DOOM");
    char* doom_argv[] = {(char*)"doom", (char*)"-iwad", (char*)"/sdcard/DOOM/DOOM1.WAD"};
    doomgeneric_Create(3, doom_argv);

    uint64_t next_frame_target_us = esp_timer_get_time();

    while (!emu_stop_requested) {
        if (gpio_get_level(BOOT_BTN_PIN) == 0) {
            emu_stop_requested = true;
            break;
        }

        doomgeneric_Tick();

        next_frame_target_us += 28571;
        uint64_t current_time_us = esp_timer_get_time();
        if (current_time_us > next_frame_target_us + 57142) next_frame_target_us = current_time_us;
        
        if (current_time_us < next_frame_target_us) {
            uint32_t delay_ms = (next_frame_target_us - current_time_us) / 1000;
            if (delay_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    bsp_display_brightness_set(0); 
    const esp_partition_t *factory_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory_part) {
        esp_ota_set_boot_partition(factory_part);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}