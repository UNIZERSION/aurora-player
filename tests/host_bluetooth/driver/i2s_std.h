#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef void *i2s_chan_handle_t;
enum {I2S_NUM_0=0,I2S_ROLE_MASTER=0,I2S_GPIO_UNUSED=-1,I2S_DATA_BIT_WIDTH_16BIT=16,I2S_SLOT_MODE_STEREO=2};
struct i2s_chan_config_t {int dma_desc_num=0,dma_frame_num=0;bool auto_clear_after_cb=false;};
struct i2s_std_config_t {int clk_cfg=0,slot_cfg=0;struct {int mclk,bclk,ws,dout,din;} gpio_cfg{};};
#define I2S_CHANNEL_DEFAULT_CONFIG(num,role) i2s_chan_config_t{}
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) (rate)
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits,mode) (bits)
esp_err_t i2s_new_channel(const i2s_chan_config_t*,i2s_chan_handle_t*,void*);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t,const i2s_std_config_t*);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_channel_write(i2s_chan_handle_t,const void*,size_t,size_t*,uint32_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
