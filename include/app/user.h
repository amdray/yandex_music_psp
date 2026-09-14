#ifndef APP_USER_H
#define APP_USER_H

// Хранит информацию о пользователе, полученную из API
typedef struct {
    int uid;
    char display_name[96];
    int subscription_active;
    char subscription_end[32];
} UserInfo;

#endif
