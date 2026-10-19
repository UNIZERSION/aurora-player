#ifdef NDEBUG
#undef NDEBUG
#endif
#include "music_library.h"
#include "favorites_store.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>
#define ESP_LOGI(tag,...) do {std::printf(__VA_ARGS__);std::putchar('\n');} while(false)

#include "audio_engine.h"      // real audio_state_t
#include "transport_policy.h"  // real Next/Previous keep-paused policy

constexpr const char *MUSIC="/sdcard/Music";
enum {PLAYER_UI_FOLDER_SELECT,FOLDER_SELECT,PLAYER_UI_LIBRARY_TAB,PLAYER_UI_SHUFFLE,PLAYER_UI_FAVORITE,PLAYER_UI_FORMAT_FILTER};
struct Command{int action,value;};
static uint32_t esp_random(){return 139;}
static bool locked;
static bool tab5_display_lock(unsigned){assert(!locked);locked=true;return true;}
static void tab5_display_unlock(){assert(locked);locked=false;}
static void close_dialog(){assert(locked);}
static unsigned seek_calls;
extern "C" esp_err_t audio_engine_seek_ms(uint32_t){++seek_calls;return ESP_OK;}
struct player_ui_track_t {const char *title,*artist;uint32_t duration;bool favorite;const char *path;};
static void player_ui_update_track(int,const player_ui_track_t*){assert(locked);}
struct Player {
    player::Settings settings;
    player::MusicLibrary library;
    player::FavoritesStore favorites{"unused-test-store",MUSIC};
    player::ShuffleBag shuffle;
    std::vector<size_t> playback_queue;
    bool playback_favorites=false;
    std::vector<int> history;
    struct {audio_state_t state=AUDIO_STOPPED;char path[240]{};unsigned position_ms=0;bool seek_supported=true;} audio;
    int current=-1;
    std::string search_query,message,last_request;
    unsigned playlist_updates=0,folder_updates=0,play_calls=0;
    bool last_paused=false;
    bool play_pending=false,resume_pending=false,pending_paused=false;
    bool reject_play=false;
    uint64_t last_state=0;
    void update_playlist(){++playlist_updates;}
    void update_folders(){++folder_updates;}
    void mark_dirty(){}
    void notice(const std::string &text){message=text;}
    bool play_path(const std::string &path,bool remember=true,uint32_t resume=0,bool paused=false){
        (void)resume;
        if(reject_play)return false;
        if(remember&&settings.shuffle&&queue_index()>=0&&track()&&track()->path!=path)history.push_back(queue_index());
        ++play_calls;last_request=path;last_paused=paused;
        current=library.find_visible(path);snprintf(audio.path,sizeof audio.path,"%s",path.c_str());
        audio.state=paused?AUDIO_PAUSED:AUDIO_PLAYING;return true;
    }
#include "folder_production.h"
};
static Player fixture(){
    Player p;
    const char *paths[]={"/sdcard/Music/Rock/one.mp3","/sdcard/Music/Rock/Live/two.wav","/sdcard/Music/Jazz/three.mp3","/sdcard/Music/Rockabilly/four.mp3"};
    for(unsigned i=0;i<4;++i){player::Track t;t.path=paths[i];t.info.format=i==1?AUDIO_FORMAT_WAV:AUDIO_FORMAT_MP3;snprintf(t.info.title,sizeof t.info.title,"track%u",i);p.library.tracks.push_back(t);}
    p.library.folders={MUSIC,"/sdcard/Music/Rock","/sdcard/Music/Rock/Live","/sdcard/Music/Jazz","/sdcard/Music/Rockabilly","/sdcard/Music/Empty"};
    snprintf(p.settings.folder,sizeof p.settings.folder,"%s",MUSIC);p.filter();p.rebuild_queue();
    return p;
}
int main(){
    // Choosing a folder while another folder's song is paused must not change
    // that song, start sound, or leave a misleading selected row in the Queue.
    auto p=fixture();const std::string jazz=p.library.tracks[2].path;
    p.play_path(jazz,false,0,true);const unsigned plays=p.play_calls;
    p.search_query="three";p.select_folder(1);
    assert(p.settings.library_tab==0&&p.search_query.empty());
    assert(p.library.visible==std::vector<size_t>({1,0}));
    assert(p.playback_queue==std::vector<size_t>({1,0}));
    assert(p.current==-1&&p.track()->path==jazz&&p.audio.state==AUDIO_PAUSED&&p.play_calls==plays);
    assert(p.message.empty()&&p.folder_updates==1);
    p.next(false);assert(p.last_request==p.library.tracks[1].path&&p.last_paused);
    // Selecting a nested Queue song keeps its explicitly selected parent scope.
    p.play(0);assert(std::string(p.settings.folder)=="/sdcard/Music/Rock");
    assert(p.playback_queue==std::vector<size_t>({1,0}));
    const auto song=p.track()->path;p.set_tab(1);assert(p.track()->path==song&&p.settings.library_tab==1);
    p.set_tab(0);assert(p.library.visible==std::vector<size_t>({1,0}));
    puts("PASS production folder selection: Files -> scoped Queue, parent includes children, paused current song survives, Next preserves pause, Queue click retains parent scope.");

    // Favorites are global and become the playback origin only when selected.
    assert(p.favorites.set(jazz,true));const auto before=p.track()->path;
    p.set_tab(2);assert(p.library.visible==std::vector<size_t>({2}));
    assert(p.track()->path==before&&std::string(p.settings.folder)=="/sdcard/Music/Rock");
    p.play(0);assert(p.last_request==jazz&&std::string(p.settings.folder)=="/sdcard/Music/Rock"&&p.playback_favorites);
    assert(p.playback_queue==std::vector<size_t>({2}));
    p.toggle_favorite(-1);assert(p.library.visible.empty()&&p.playback_queue.empty()&&p.track()->path==jazz);
    p.set_tab(0);assert(p.library.visible==std::vector<size_t>({1,0}));
    puts("PASS global favorites: playback origin is Favorites; browsing preserves the folder and removal leaves currently playing metadata available.");

    p.settings.shuffle=1;p.settings.repeat=2;p.select_folder(1);
    assert(p.playback_favorites&&p.playback_queue.empty());
    p.play(0);assert(!p.playback_favorites); // Selecting Queue music explicitly leaves Favorites.
    for(unsigned i=0;i<20;++i){p.next(false);assert(player::valid_music_path(p.last_request,"/sdcard/Music/Rock"));}
    p.settings.format_filter=AUDIO_FORMAT_AAC;p.filter();const unsigned before_empty=p.play_calls;
    p.select_folder(5);assert(p.library.visible.empty()&&p.playback_queue.empty());
    assert(p.message=="No tracks match this folder and file filter.");
    p.next(false);assert(p.play_calls==before_empty);
    const auto folder=std::string(p.settings.folder);p.select_folder(999);
    assert(std::string(p.settings.folder)==folder&&p.play_calls==before_empty);
    puts("PASS scoped shuffle (20 transitions), empty/filter status and invalid folder index; no filesystem, UI or actual audio side effects exercised.");
    // Exact state established by real play_path after asynchronous acceptance,
    // before the next engine snapshot: current/last_path/pending are new, while
    // the snapshot is still STOPPED. A subsequent folder command can be queued
    // in the same controller loop. Loading later must still match track().
    auto pending=fixture();const auto requested=pending.library.tracks[0].path;
    pending.current=pending.library.find_visible(requested);pending.play_pending=true;
    snprintf(pending.settings.last_path,sizeof pending.settings.last_path,"%s",requested.c_str());
    pending.select_folder(3);
    const bool pending_selection_ok=pending.track()&&pending.track()->path==requested;
    snprintf(pending.audio.path,sizeof pending.audio.path,"%s",requested.c_str());
    pending.audio.state=AUDIO_PLAYING;
    if(!pending.track()||pending.track()->path!=requested){
        printf("FAIL pending-play + folder switch: requested=%s, resolved=%s; AUDIO_EVENT_LOADED cannot clear play_pending.\n",requested.c_str(),pending.track()?pending.track()->path.c_str():"<none>");
        return 2;
    }
    assert(pending_selection_ok&&pending.current==-1);
    puts("PASS pending play survives folder switch before engine snapshot; loaded audio still resolves to the requested current track.");
    for(audio_state_t old_state:{AUDIO_PLAYING,AUDIO_PAUSED}){
        auto changing=fixture();const auto old=changing.library.tracks[2].path;
        const auto requested_new=changing.library.tracks[0].path;
        snprintf(changing.audio.path,sizeof changing.audio.path,"%s",old.c_str());
        changing.audio.state=old_state;
        changing.current=changing.library.find_visible(requested_new);changing.play_pending=true;
        snprintf(changing.settings.last_path,sizeof changing.settings.last_path,"%s",requested_new.c_str());
        changing.select_folder(4); // A=Jazz -> pending B=Rock -> browse C=Rockabilly.
        assert(changing.current==-1&&changing.track()&&changing.track()->path==requested_new);
        assert(changing.audio.state==old_state&&std::string(changing.audio.path)==old&&changing.play_calls==0);
        snprintf(changing.audio.path,sizeof changing.audio.path,"%s",requested_new.c_str());
        changing.audio.state=AUDIO_PLAYING;
        assert(changing.track()->path==changing.audio.path); // LOADED can clear pending.
        changing.play_pending=false;
        assert(changing.track()->path==requested_new); // Then resolves through actual audio.
    }
    puts("PASS stale playing/paused audio.path A -> requested B -> folder C; pending B wins before snapshot, loaded B matches, clearing pending retains B.");

    auto fav=fixture();
    for(size_t n:{0U,2U,3U})assert(fav.favorites.set(fav.library.tracks[n].path,true));
    fav.set_tab(2);fav.play(0);
    const auto favorite_queue=std::vector<size_t>({2,0,3});
    assert(fav.playback_favorites&&fav.playback_queue==favorite_queue);
    fav.next(true);assert(fav.last_request==fav.library.tracks[0].path);
    fav.previous();assert(fav.last_request==fav.library.tracks[2].path);
    // Browse a folder, hide every favorite with search/format, then toggle
    // Shuffle. None of those view-only operations may adopt all SD music.
    fav.select_folder(2);assert(fav.library.visible==std::vector<size_t>({1}));
    assert(fav.playback_favorites&&fav.playback_queue==favorite_queue);
    fav.search_query="not a title";fav.set_format(AUDIO_FORMAT_WAV);
    assert(fav.library.visible.empty()&&fav.playback_queue==favorite_queue);
    fav.set_tab(0);fav.toggle_shuffle(0);fav.settings.repeat=2;
    for(unsigned i=0;i<100;++i){
        const auto old=fav.track()->path;
        fav.next(i%2==0);assert(fav.favorites.contains(fav.last_request)&&fav.last_request!=old);
        if(i%5==0){fav.previous();assert(fav.last_request==old);}
    }
    fav.toggle_shuffle(0);assert(!fav.settings.shuffle&&fav.playback_queue==favorite_queue);
    for(unsigned i=0;i<20;++i){fav.next(true);assert(fav.favorites.contains(fav.last_request));fav.previous();assert(fav.favorites.contains(fav.last_request));}
    puts("PASS favorites spanning three folders: manual/automatic next, previous and shuffle stay scoped across Files/Queue/search/format view changes (120 transitions).");

    // Removing the playing favorite lets it finish, but removes it immediately
    // from future selection, including Repeat one and Previous's restart path.
    fav.toggle_shuffle(0);fav.next(false);assert(!fav.history.empty());
    const auto removed=fav.track()->path;fav.toggle_favorite(-1);
    assert(!fav.favorites.contains(removed)&&fav.history.empty()&&fav.playback_queue.size()==2);
    fav.settings.repeat=1;fav.next(true);assert(fav.last_request!=removed&&fav.favorites.contains(fav.last_request));
    fav.audio.position_ms=5000;const auto removed_again=fav.track()->path;fav.toggle_favorite(-1);
    const unsigned seeks_before=seek_calls;fav.previous();
    assert(seek_calls==seeks_before&&fav.last_request!=removed_again&&fav.favorites.contains(fav.last_request));
    assert(fav.playback_queue.size()==1);
    fav.settings.repeat=2;fav.next(true);assert(fav.favorites.contains(fav.last_request));
    const auto only=fav.last_request;fav.settings.repeat=0;const auto plays_before=fav.play_calls;
    fav.next(true);assert(fav.play_calls==plays_before); // Single item, no repeat.
    fav.settings.repeat=2;fav.toggle_favorite(-1);const auto empty_plays=fav.play_calls;
    assert(fav.playback_queue.empty());
    fav.next(true);fav.next(false);fav.previous();
    fav.settings.repeat=1;fav.next(true);
    assert(fav.play_calls==empty_plays&&fav.last_request==only);
    puts("PASS live favorite removal clears obsolete shuffle history, repeat-one skips removed current, single/empty lists never fall back to other tracks.");

    // A rejected Queue selection must not change an existing Favorites scope.
    fav.search_query.clear();fav.set_format(0);fav.select_folder(1);
    fav.reject_play=true;assert(!fav.play(0)&&fav.playback_favorites&&fav.playback_queue.empty());
    fav.reject_play=false;assert(fav.play(0)&&!fav.playback_favorites);
    assert(fav.playback_queue==std::vector<size_t>({1,0}));
    puts("PASS only a successful explicit track selection changes playback origin; ordinary parent-folder playback remains intact.");
}

