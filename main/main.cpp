#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
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

// =======================================================
// FILA DE EVENTOS DO TECLADO VIRTUAL
// =======================================================
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

// =======================================================
// OBRIGAÇÕES DO MOTOR DOOMGENERIC
// =======================================================
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
            // Centralização Exata para Tela 410x502 (X=45, Y=151)
            area.x1 = 45; area.y1 = 151 + (chunk * CHUNK_LINES);
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
    
    // [NOVO] Atraso vital para impedir a "Corrida" contra a placa de vídeo
    vTaskDelay(pdMS_TO_TICKS(500)); 

    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    if (i2c_bus == NULL) {
        i2c_master_bus_config_t i2c_mst_config = {};
        i2c_mst_config.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_mst_config.i2c_port = -1;
        i2c_mst_config.scl_io_num = GPIO_NUM_14;
        i2c_mst_config.sda_io_num = GPIO_NUM_15;
        i2c_mst_config.glitch_ignore_cnt = 7;
        i2c_mst_config.flags.enable_internal_pullup = true;
        i2c_new_master_bus(&i2c_mst_config, &i2c_bus);
    }

    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        lv_obj_t * scr = lv_scr_act(); // [CORRIGIDO] Chamada mais segura do LVGL
        lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
        
        // ==========================================
        // GAMEPAD VIRTUAL PARA TELA 410x502
        // ==========================================
        #define KEY_RIGHTARROW 0xae
        #define KEY_LEFTARROW  0xac
        #define KEY_UPARROW    0xad
        #define KEY_DOWNARROW  0xaf
        #define KEY_RCTRL      (0x80+0x1d) 
        
        // BOTÕES DE AÇÃO SUPERIORES (Y = 40, mais espaço para os dedos)
        create_virtual_btn(scr, 20,  40, 80, 70, "FIRE",  KEY_RCTRL, lv_color_hex(0xFF3333));
        create_virtual_btn(scr, 115, 40, 80, 70, "USE",   ' ',       lv_color_hex(0x33FF33)); 
        create_virtual_btn(scr, 210, 40, 80, 70, "ENTER", 13,        lv_color_hex(0x3333FF)); 
        create_virtual_btn(scr, 305, 40, 85, 70, "ESC",   27,        lv_color_hex(0xAAAAAA)); 
        
        // D-PAD INFERIOR (Formato de Cruz espaçosa para não errar o toque)
        int cx = 205; // Metade da tela de 410
        int dpad_y = 430; // Y Base
        create_virtual_btn(scr, cx - 110, dpad_y - 30, 70, 70, LV_SYMBOL_LEFT,  KEY_LEFTARROW,  lv_color_hex(0x555555));
        create_virtual_btn(scr, cx + 40,  dpad_y - 30, 70, 70, LV_SYMBOL_RIGHT, KEY_RIGHTARROW, lv_color_hex(0x555555));
        create_virtual_btn(scr, cx - 35,  dpad_y - 75, 70, 70, LV_SYMBOL_UP,    KEY_UPARROW,    lv_color_hex(0x555555));
        create_virtual_btn(scr, cx - 35,  dpad_y + 5,  70, 70, LV_SYMBOL_DOWN,  KEY_DOWNARROW,  lv_color_hex(0x555555));
        
        // Botões de trocar arma/esquivar nos cantos da cruz
        create_virtual_btn(scr, 10,  420, 60, 60, "<", ',', lv_color_hex(0x888888));
        create_virtual_btn(scr, 340, 420, 60, 60, ">", '.', lv_color_hex(0x888888));

        bsp_display_unlock();
    }
    bsp_display_brightness_set(80);

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
            while (esp_timer_get_time() < next_frame_target_us) { taskYIELD(); }
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    ESP_LOGW(TAG, "Jogo Encerrado! Devolvendo o controle ao Relogio...");
    bsp_display_brightness_set(0); 
    const esp_partition_t *factory_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory_part) {
        esp_ota_set_boot_partition(factory_part);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}