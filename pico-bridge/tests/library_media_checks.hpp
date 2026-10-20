int main() {
    BluetoothA2DPSource bt;
    bt.get_data_in_frames_cb=[](Frame*,int32_t frames)->int32_t {
        if(frames<0){assert(frames==-1);return 0;}
        assert(frames==4);return 2;
    };
    assert(bt.get_audio_data_volume(nullptr,-1)==0&&bt.volume.calls==0);
    uint8_t data[16]{};assert(bt.get_audio_data_volume(data,16)==8);
    assert(bt.volume.calls==1&&bt.volume.lastLength==8);
    bt.set_media_active(true);bt.service_media();bt.service_media();assert(queue_calls==1);
    bt.tick();assert(commands==std::vector<int>{ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY});
    bt.ack(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);assert(commands.back()==ESP_A2D_MEDIA_CTRL_START);
    bt.audio(ESP_A2D_AUDIO_STATE_STARTED);bt.ack(ESP_A2D_MEDIA_CTRL_START);
    bt.set_media_active(false);bt.tick();assert(commands.back()==ESP_A2D_MEDIA_CTRL_SUSPEND);
    bt.audio(1);bt.ack(ESP_A2D_MEDIA_CTRL_SUSPEND);
    assert(bt.s_a2d_state==APP_AV_STATE_CONNECTED);
    const auto paused=commands.size();
    for(clock_ms=0;clock_ms<300000;clock_ms+=100) {
        bt.tick();bt.bt_app_av_state_connected_hdlr(BT_APP_HEART_BEAT_EVT,nullptr);
    }
    assert(commands.size()==paused);
    bt.set_media_active(true);bt.tick();assert(commands.back()==ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    bt.ack(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);assert(commands.back()==ESP_A2D_MEDIA_CTRL_START);
    bt.set_media_active(false);bt.tick();const auto pending=commands.size();
    bt.ack(ESP_A2D_MEDIA_CTRL_START);assert(commands.size()==pending+1 && commands.back()==ESP_A2D_MEDIA_CTRL_SUSPEND);
    bt.set_media_active(true);bt.tick();assert(commands.size()==pending+1);
    bt.ack(ESP_A2D_MEDIA_CTRL_SUSPEND);assert(commands.back()==ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    bt.ack(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);bt.audio(2);bt.ack(ESP_A2D_MEDIA_CTRL_START);
    api_result=1;bt.set_media_active(false);bt.tick();const auto failed=commands.size();
    clock_ms+=99;bt.tick();assert(commands.size()==failed);
    api_result=0;++clock_ms;bt.tick();assert(commands.size()==failed+1 && commands.back()==ESP_A2D_MEDIA_CTRL_SUSPEND);
    bt.ack(ESP_A2D_MEDIA_CTRL_SUSPEND,ESP_A2D_MEDIA_CTRL_ACK_BUSY);
    clock_ms+=100;bt.tick();assert(commands.back()==ESP_A2D_MEDIA_CTRL_SUSPEND);
    bt.ack(ESP_A2D_MEDIA_CTRL_SUSPEND);assert(bt.s_a2d_state==APP_AV_STATE_CONNECTED);
    bt.media_tick_queued=false;queue_success=false;bt.service_media();assert(!bt.media_tick_queued);
    queue_success=true;bt.service_media();assert(bt.media_tick_queued);
    bt.set_media_active(true);bt.tick();bt.ack(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    bt.audio(2);bt.ack(ESP_A2D_MEDIA_CTRL_START);
    bt.recover_media();bt.tick();assert(commands.back()==ESP_A2D_MEDIA_CTRL_SUSPEND);
    const auto recovering=commands.size();bt.tick();assert(commands.size()==recovering);
    bt.audio(1);bt.ack(ESP_A2D_MEDIA_CTRL_SUSPEND);
    assert(commands.back()==ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    bt.ack(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);bt.audio(2);bt.ack(ESP_A2D_MEDIA_CTRL_START);
    assert(bt.s_a2d_state==APP_AV_STATE_CONNECTED && bt.media_ready);
    // STOP between scheduling and executing recovery cannot restart playback.
    bt.recover_media();bt.set_media_active(false);bt.tick();
    bt.ack(ESP_A2D_MEDIA_CTRL_SUSPEND);const auto stopped=commands.size();
    bt.recover_media();bt.tick();assert(commands.size()==stopped && !bt.media_ready);
    std::puts("PASS real BluetoothA2DPSource methods: SDK flush -1, actual filled volume length, command mapping, no disconnect, queue coalescing/failure, async state/ACK order, fast reverse, immediate API failure, BUSY retry and serialized recovery");
}
