#include "services/locale.h"

#include <string.h>
#include <stdio.h>
#include "core/logger.h"

// Locale string tables - компилируемые таблицы для каждого языка
// Формат: [LOCALE_KEY][LANGUAGE] = "string"

static const char *s_locale_strings[LOCALE_COUNT][LOCALE_LANG_COUNT] = {
    // Splash screen
    [LOCALE_SPLASH_INITIALIZING] = { "Initializing...", "Инициализация..." },
    [LOCALE_SPLASH_STORAGE] = { "storage", "хранилище" },
    [LOCALE_SPLASH_CACHE] = { "cache", "кэш" },
    [LOCALE_SPLASH_INDEX] = { "index", "индекс" },
    [LOCALE_SPLASH_LOCALE] = { "locale", "локализация" },
    [LOCALE_SPLASH_TOKEN] = { "token", "токен" },
    [LOCALE_SPLASH_NET] = { "net", "сеть" },
    [LOCALE_SPLASH_AUTH] = { "auth", "авторизация" },
    [LOCALE_SPLASH_TOKEN_ERROR] = { "token error", "ошибка токена" },
    [LOCALE_SPLASH_NET_ERROR] = { "net error", "ошибка сети" },
    [LOCALE_SPLASH_AUTH_ERROR] = { "auth error", "ошибка авторизации" },
    [LOCALE_SPLASH_RETRY_PROMPT] = { "X: retry", "X: повторить" },
    [LOCALE_SPLASH_EXIT_PROMPT] = { "START+SELECT: exit", "START+SELECT: выход" },
    
    // Menu
    [LOCALE_MENU_TITLE] = { "Menu", "Меню" },
    [LOCALE_MENU_NOW_PLAYING] = { "Now Playing", "Сейчас играет" },
    [LOCALE_MENU_ALBUMS] = { "Albums", "Альбомы" },
    [LOCALE_MENU_PLAYLISTS] = { "Playlists", "Плейлисты" },
    [LOCALE_MENU_ACCOUNT] = { "Account", "Аккаунт" },
    [LOCALE_MENU_ARTISTS] = { "Artists", "Исполнители" },
    [LOCALE_MENU_SELECT_PROMPT] = { "X: select", "X: выбрать" },
    [LOCALE_MENU_BACK_PROMPT] = { "O: back", "O: назад" },
    [LOCALE_MENU_EXIT_PROMPT] = { "START+SELECT: exit", "START+SELECT: выход" },
    
    // Screens
    [LOCALE_SCREEN_NOW_PLAYING] = { "Now Playing", "Сейчас играет" },
    [LOCALE_SCREEN_ALBUMS] = { "Albums", "Альбомы" },
    [LOCALE_SCREEN_PLAYLIST_LIST] = { "Playlists", "Плейлисты" },
    [LOCALE_SCREEN_TRACK_LIST] = { "Track List", "Список треков" },
    [LOCALE_SCREEN_TRACK_LIST_OF_PLAYLIST] = { "Track List of Playlist %s", "Список треков плейлиста %s" },
    [LOCALE_SCREEN_ACCOUNT] = { "Account", "Аккаунт" },
    [LOCALE_SCREEN_ARTIST] = { "Artist", "Исполнитель" },
    [LOCALE_ARTIST_PLAY_PROMPT] = { "[]: play top", "[]: играть топ" },
    [LOCALE_SCREEN_EMPTY] = { "(empty)", "(пусто)" },
    [LOCALE_SCREEN_PLACEHOLDER] = { "(placeholder)", "(заглушка)" },
    
    // Account screen
    [LOCALE_ACCOUNT_USER] = { "User", "Пользователь" },
    [LOCALE_ACCOUNT_UID] = { "UID", "UID" },
    [LOCALE_ACCOUNT_SUBSCRIPTION_ACTIVE] = { "Subscription: Active", "Подписка: Активна" },
    [LOCALE_ACCOUNT_SUBSCRIPTION_INACTIVE] = { "Subscription: Inactive", "Подписка: Неактивна" },
    [LOCALE_ACCOUNT_SUBSCRIPTION_EXPIRES] = { "Expires", "Истекает" },
    [LOCALE_ACCOUNT_LOAD_ERROR] = { "Could not load user data.", "Не удалось загрузить данные пользователя." },
    
    // Playlists
    [LOCALE_PLAYLIST_OPEN_PROMPT] = { "X: open playlist", "X: открыть плейлист" },
    [LOCALE_PLAYLIST_BACK_PROMPT] = { "O: back", "O: назад" },

    // Playlist load errors
    [LOCALE_LOAD_ERR_RATELIMIT] = { "Too many requests, try later", "Слишком много запросов, позже" },
    [LOCALE_LOAD_ERR_NOCONN]    = { "No connection", "Нет соединения" },
    [LOCALE_LOAD_ERR_GENERIC]   = { "Load error", "Ошибка загрузки" },
    
    // Tracks
    [LOCALE_TRACK_PLAY_PROMPT] = { "X: play", "X: воспроизвести" },
    [LOCALE_TRACK_BACK_PROMPT] = { "O: back", "O: назад" },
    [LOCALE_NOW_PLAYING_TOGGLE_PROMPT] = { "START: play/pause", "START: играть/пауза" },
    [LOCALE_NOW_PLAYING_STOP_PROMPT] = { "[]: stop", "[]: стоп" },
    [LOCALE_NOW_PLAYING_LIKE_PROMPT] = { "/\\: like", "/\\: лайк" },
    
    // Albums
    [LOCALE_ALBUM_OPEN_PROMPT] = { "X: open album", "X: открыть альбом" },
    [LOCALE_ALBUM_BACK_PROMPT] = { "O: back", "O: назад" },
    
    // Playlist info
    [LOCALE_PLAYLIST_TRACKS] = { "tracks", "треков" },
    [LOCALE_PLAYLIST_LIKED_TRACKS] = { "Liked tracks", "Мне нравится" },
    
    // Track info
    [LOCALE_TRACK_EXPLICIT] = { "[E]", "[E]" },

    // Track download flow
    [LOCALE_TRACK_DOWNLOAD_PROMPT]  = { "[] download",      "[] скачать" },
    [LOCALE_TRACK_DOWNLOAD_ALREADY] = { "Already cached",   "Уже скачан" },
    [LOCALE_TRACK_DOWNLOADING]      = { "Downloading...",   "Скачивание..." },
    [LOCALE_TRACK_DOWNLOAD_OK]      = { "Download OK",      "Скачано" },
    [LOCALE_TRACK_DOWNLOAD_ERROR]   = { "Download failed",  "Ошибка загрузки" },

    // Playlist screen tabs
    [LOCALE_TAB_MY_PLAYLISTS]    = { "Mine",        "Мои" },
    [LOCALE_TAB_LIKED_PLAYLISTS] = { "Liked",       "Лайки" },
    [LOCALE_PLAYLIST_TAB_PROMPT] = { "L/R: tab",    "L/R: вкладка" },

    // Net info screen
    [LOCALE_MENU_NET_INFO]          = { "Net Info",       "Сеть" },
    [LOCALE_SCREEN_NET_INFO]        = { "Net Info",       "Информация о сети" },
    [LOCALE_NET_INFO_NOT_CONNECTED] = { "Not connected",  "Нет соединения" },
    [LOCALE_NET_INFO_PROFILE]       = { "Profile",        "Профиль" },
    [LOCALE_NET_INFO_SSID]          = { "SSID",           "SSID" },
    [LOCALE_NET_INFO_BSSID]         = { "BSSID",          "BSSID" },
    [LOCALE_NET_INFO_SECURITY]      = { "Security",       "Защита" },
    [LOCALE_NET_INFO_CHANNEL]       = { "Channel",        "Канал" },
    [LOCALE_NET_INFO_SIGNAL]        = { "Signal",         "Сигнал" },
    [LOCALE_NET_INFO_IP]            = { "IP",             "IP" },
    [LOCALE_NET_INFO_SUBNET]        = { "Subnet",         "Маска" },
    [LOCALE_NET_INFO_GATEWAY]       = { "Gateway",        "Шлюз" },
    [LOCALE_NET_INFO_DNS1]          = { "DNS1",           "DNS1" },
    [LOCALE_NET_INFO_DNS2]          = { "DNS2",           "DNS2" },
    [LOCALE_ACCOUNT_LOGIN_PROMPT] = { "X: phone login", "X: вход по коду" },
    [LOCALE_ACCOUNT_LOGOUT_PROMPT] = { "[]: logout", "[]: выйти" },
    [LOCALE_MENU_EQ]        = { "Equalizer", "Эквалайзер" },
    [LOCALE_MENU_WAVE]      = { "My wave", "Моя волна" },
    [LOCALE_EQ_TITLE]       = { "Equalizer", "Эквалайзер" },
    [LOCALE_EQ_PRESET]      = { "Profile", "Профиль" },
    [LOCALE_EQ_EDIT_PROMPT] = { "Up/Down: band Left/Right: dB", "Вверх/вниз: полоса Влево/вправо: дБ" },
    [LOCALE_EQ_OFF]       = { "Off", "Выкл" },
    [LOCALE_EQ_ROCK]      = { "Rock", "Рок" },
    [LOCALE_EQ_POP]       = { "Pop", "Поп" },
    [LOCALE_EQ_JAZZ]      = { "Jazz", "Джаз" },
    [LOCALE_EQ_CLASSICAL] = { "Classical", "Классика" },
    [LOCALE_EQ_BASS]      = { "Bass", "Бас" },
    [LOCALE_EQ_TREBLE]    = { "Treble", "Высокие" },
    [LOCALE_EQ_VOCAL]     = { "Vocal", "Вокал" },
    [LOCALE_EQ_CUSTOM]    = { "Custom", "Свой" },
    [LOCALE_SCREEN_WAVE]  = { "My wave", "Моя волна" },
    [LOCALE_WAVE_LOADING] = { "Loading wave...", "Загружаю волну..." },
    [LOCALE_WAVE_ERROR]   = { "Wave unavailable", "Волна недоступна" },
    [LOCALE_LOGIN_TITLE]        = { "Phone code login", "Вход по коду" },
    [LOCALE_LOGIN_REQUEST]      = { "Requesting login code...", "Запрос кода..." },
    [LOCALE_LOGIN_OPEN]         = { "On your phone open:", "На телефоне открой:" },
    [LOCALE_LOGIN_ENTER_CODE]   = { "and enter the code:", "и введи код:" },
    [LOCALE_LOGIN_LEFT]         = { "Left: %d sec. Waiting... (try %d)", "Осталось: %d сек. Жду... (попытка %d)" },
    [LOCALE_LOGIN_OK]           = { "Login ok!", "Вход выполнен!" },
    [LOCALE_LOGIN_RETRY_PROMPT] = { "X: retry", "X: повтор" },
};

static LocaleLang s_current_lang = LOCALE_LANG_EN;

static LocaleLang lang_code_to_enum(const char *lang_code)
{
    if (!lang_code) {
        return LOCALE_LANG_EN;
    }
    
    if (strcmp(lang_code, "ru") == 0 || strcmp(lang_code, "RU") == 0) {
        return LOCALE_LANG_RU;
    }
    
    // Default to English
    return LOCALE_LANG_EN;
}

int locale_init(const char *lang_code)
{
    s_current_lang = lang_code_to_enum(lang_code);
    logLine("locale: initialized with lang=%s (enum=%d)\n", 
            lang_code ? lang_code : "en", (int)s_current_lang);
    return 0;
}

void locale_shutdown(void)
{
    logLine("locale: shutdown\n");
}

const char *locale_get(LocaleKey key)
{
    if (key < 0 || key >= LOCALE_COUNT) {
        logLine("locale: invalid key %d\n", (int)key);
        return "?";
    }
    
    const char *str = s_locale_strings[key][s_current_lang];
    if (!str || str[0] == '\0') {
        if (s_current_lang != LOCALE_LANG_EN) {
            str = s_locale_strings[key][LOCALE_LANG_EN];
        }
        if (!str || str[0] == '\0') {
            return "?";
        }
    }
    
    return str;
}

LocaleLang locale_get_current_lang(void)
{
    return s_current_lang;
}

int locale_set_lang(const char *lang_code)
{
    s_current_lang = lang_code_to_enum(lang_code);
    logLine("locale: language changed to %s\n", lang_code ? lang_code : "en");
    return 0;
}
