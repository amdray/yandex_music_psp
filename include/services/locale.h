#ifndef YM_SERVICES_LOCALE_H
#define YM_SERVICES_LOCALE_H

// Locale key enumeration - все строки приложения
typedef enum {
    // Splash screen
    LOCALE_SPLASH_INITIALIZING,
    LOCALE_SPLASH_STORAGE,
    LOCALE_SPLASH_CACHE,
    LOCALE_SPLASH_INDEX,
    LOCALE_SPLASH_LOCALE,
    LOCALE_SPLASH_TOKEN,
    LOCALE_SPLASH_NET,
    LOCALE_SPLASH_AUTH,
    LOCALE_SPLASH_TOKEN_ERROR,
    LOCALE_SPLASH_NET_ERROR,
    LOCALE_SPLASH_AUTH_ERROR,
    LOCALE_SPLASH_RETRY_PROMPT,
    LOCALE_SPLASH_EXIT_PROMPT,
    
    // Menu
    LOCALE_MENU_TITLE,
    LOCALE_MENU_NOW_PLAYING,
    LOCALE_MENU_ALBUMS,
    LOCALE_MENU_PLAYLISTS,
    LOCALE_MENU_ACCOUNT,
    LOCALE_MENU_ARTISTS,
    LOCALE_MENU_SELECT_PROMPT,
    LOCALE_MENU_BACK_PROMPT,
    LOCALE_MENU_EXIT_PROMPT,
    
    // Screens
    LOCALE_SCREEN_NOW_PLAYING,
    LOCALE_SCREEN_ALBUMS,
    LOCALE_SCREEN_PLAYLIST_LIST,
    LOCALE_SCREEN_TRACK_LIST,
    LOCALE_SCREEN_TRACK_LIST_OF_PLAYLIST,  // "Список треков плейлиста %s"
    LOCALE_SCREEN_ACCOUNT,
    LOCALE_SCREEN_ARTIST,
    LOCALE_ARTIST_PLAY_PROMPT,
    LOCALE_SCREEN_EMPTY,
    LOCALE_SCREEN_PLACEHOLDER,
    
    // Account screen
    LOCALE_ACCOUNT_USER,
    LOCALE_ACCOUNT_UID,
    LOCALE_ACCOUNT_SUBSCRIPTION_ACTIVE,
    LOCALE_ACCOUNT_SUBSCRIPTION_INACTIVE,
    LOCALE_ACCOUNT_SUBSCRIPTION_EXPIRES,
    LOCALE_ACCOUNT_LOAD_ERROR,
    
    // Playlists
    LOCALE_PLAYLIST_OPEN_PROMPT,
    LOCALE_PLAYLIST_BACK_PROMPT,

    // Playlist load errors (show the cause instead of a blank screen)
    LOCALE_LOAD_ERR_RATELIMIT,
    LOCALE_LOAD_ERR_NOCONN,
    LOCALE_LOAD_ERR_GENERIC,
    
    // Tracks
    LOCALE_TRACK_PLAY_PROMPT,
    LOCALE_TRACK_BACK_PROMPT,
    LOCALE_NOW_PLAYING_TOGGLE_PROMPT,
    LOCALE_NOW_PLAYING_STOP_PROMPT,
    LOCALE_NOW_PLAYING_LIKE_PROMPT,
    
    // Albums
    LOCALE_ALBUM_OPEN_PROMPT,
    LOCALE_ALBUM_BACK_PROMPT,
    
    // Playlist info
    LOCALE_PLAYLIST_TRACKS,
    LOCALE_PLAYLIST_LIKED_TRACKS,
    
    // Track info
    LOCALE_TRACK_EXPLICIT,  // "[E]" label for explicit content

    // Track download flow
    LOCALE_TRACK_DOWNLOAD_PROMPT,    // button hint: "[] download"
    LOCALE_TRACK_DOWNLOAD_ALREADY,   // status: file already in cache
    LOCALE_TRACK_DOWNLOADING,        // status: download in progress
    LOCALE_TRACK_DOWNLOAD_OK,        // status: download finished ok
    LOCALE_TRACK_DOWNLOAD_ERROR,     // status: download failed

    // Playlist screen tabs
    LOCALE_TAB_MY_PLAYLISTS,         // tab label: personal playlists
    LOCALE_TAB_LIKED_PLAYLISTS,      // tab label: liked playlists
    LOCALE_PLAYLIST_TAB_PROMPT,      // button hint: L/R switch tab

    // Net info screen
    LOCALE_MENU_NET_INFO,
    LOCALE_SCREEN_NET_INFO,
    LOCALE_NET_INFO_NOT_CONNECTED,
    LOCALE_NET_INFO_PROFILE,
    LOCALE_NET_INFO_SSID,
    LOCALE_NET_INFO_BSSID,
    LOCALE_NET_INFO_SECURITY,
    LOCALE_NET_INFO_CHANNEL,
    LOCALE_NET_INFO_SIGNAL,
    LOCALE_NET_INFO_IP,
    LOCALE_NET_INFO_SUBNET,
    LOCALE_NET_INFO_GATEWAY,
    LOCALE_NET_INFO_DNS1,
    LOCALE_NET_INFO_DNS2,

    // Profile screen: phone-code login (ya_auth)
    LOCALE_ACCOUNT_LOGIN_PROMPT,
    LOCALE_ACCOUNT_LOGOUT_PROMPT,
    LOCALE_LOGIN_TITLE,
    LOCALE_LOGIN_REQUEST,
    LOCALE_LOGIN_OPEN,
    LOCALE_LOGIN_ENTER_CODE,
    LOCALE_LOGIN_LEFT,
    LOCALE_LOGIN_OK,
    LOCALE_LOGIN_RETRY_PROMPT,

    // Equalizer
    LOCALE_MENU_EQ,
    LOCALE_MENU_WAVE,
    LOCALE_EQ_TITLE,
    LOCALE_EQ_PRESET,
    LOCALE_EQ_EDIT_PROMPT,
    LOCALE_EQ_OFF,
    LOCALE_EQ_ROCK,
    LOCALE_EQ_POP,
    LOCALE_EQ_JAZZ,
    LOCALE_EQ_CLASSICAL,
    LOCALE_EQ_BASS,
    LOCALE_EQ_TREBLE,
    LOCALE_EQ_VOCAL,
    LOCALE_EQ_CUSTOM,

    // My wave
    LOCALE_SCREEN_WAVE,
    LOCALE_WAVE_LOADING,
    LOCALE_WAVE_ERROR,

    // Count
    LOCALE_COUNT
} LocaleKey;

// Language codes
typedef enum {
    LOCALE_LANG_EN,  // English (default)
    LOCALE_LANG_RU,  // Russian
    LOCALE_LANG_COUNT
} LocaleLang;

// Initialize locale system with language code ("en", "ru", etc.)
int locale_init(const char *lang_code);

// Shutdown locale system
void locale_shutdown(void);

// Get localized string by key (returns English if key not found)
const char *locale_get(LocaleKey key);

// Get current language
LocaleLang locale_get_current_lang(void);

// Set language at runtime (if supported)
int locale_set_lang(const char *lang_code);

#endif // YM_SERVICES_LOCALE_H
