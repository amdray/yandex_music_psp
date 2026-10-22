#ifndef YM_SERVICES_YA_QR_AUTH_H
#define YM_SERVICES_YA_QR_AUTH_H

// Вход по QR через Яндекс Паспорт (magic-link, как «Войти по QR» на
// passport.yandex.ru). Не нужен ни ввод кода, ни пароль на PSP:
//  1. GET  passport.yandex.ru/pwl-yandex            -> __CSRF__ + cookies
//  2. POST .../api/passport/auth/password/submit    -> track_id (JSON целиком)
//  3. POST .../api/passport/auth/magic/code         -> link (рисуем QR)
//  4. опрос POST .../auth/magic/code/status до state=otp_auth_finished
//  5. POST .../api/passport/sessions/get_session     -> Session_id cookies
//  6. POST mobileproxy.passport.yandex.net/1/bundle/oauth/token_by_sessionid
//     (Ya-Client-Cookie)                            -> x_token
//  7. POST oauth.mobile.yandex.net/1/token grant_type=x-token
//     (client_id приложения Музыки)                 -> токен Музыки
// Источник протокола: AlexxIT/YandexStation core/yandex_session.py
// (get_qr / login_qr / login_cookies / get_music_token).
//
// Состояние флоу (cookies, csrf, track) — статическое, один вход за раз.
// Все вызовы блокирующие (TLS), звать из рабочего потока.

#define YA_QR_LINK_MAX 384

// Шаги 1-3. 0 = ок, link заполнен. <0 = ошибка (err — коротко для экрана).
int ya_qr_begin(char *link, int link_sz, char *err, int err_sz);

// Шаги 4-7. 1 = токен Музыки в token; 0 = ещё не подтвердили (опросить
// позже); -2 = QR протух/отклонён (начать заново); иные <0 = ошибка сети/API.
int ya_qr_poll(char *token, int token_sz, char *err, int err_sz);

// Стереть cookies/csrf/track (после входа или отмены).
void ya_qr_reset(void);

#endif
