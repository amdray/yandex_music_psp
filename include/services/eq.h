#ifndef YM_SERVICES_EQ_H
#define YM_SERVICES_EQ_H

// Программный 7-полосный эквалайзер (биквады RBJ, float).
//
// Врезка: audio_player вызывает eq_process() для каждого декодированного
// чанка ПЕРЕД sceAudioSRCOutputBlocking. Потокобезопасность: аудиопоток
// только читает коэффициенты, UI только пишет; разрыв слова безвреден
// (коэффициенты всегда валидные числа, указателей нет).

#define EQ_BANDS 7
#define EQ_GAIN_MIN_DB (-12.0f)
#define EQ_GAIN_MAX_DB (+12.0f)

typedef enum {
    EQ_PRESET_OFF = 0,
    EQ_PRESET_ROCK,
    EQ_PRESET_POP,
    EQ_PRESET_JAZZ,
    EQ_PRESET_CLASSICAL,
    EQ_PRESET_BASS,
    EQ_PRESET_TREBLE,
    EQ_PRESET_VOCAL,
    EQ_PRESET_CUSTOM,
    EQ_PRESET_COUNT
} EqPreset;

// Полосы: 60 LS, 150/400/1k/2.4k/6k PK, 15k HS.
float eq_band_freq(int band);

// init читает config/eq.cfg (нет файла = Off). reset чистит линии
// задержки (смена трека/профиля от старых хвостов не щёлкает).
// save пишет конфиг (холодный путь, вызывать явно).
void eq_init(void);
void eq_save(void);
void eq_reset(void);

int eq_set_preset(int preset);   // кламп, возврат применённого
int eq_next_preset(void);        // для кнопки NOTE (цикл)
int eq_prev_preset(void);
int eq_get_preset(void);

// Кастомный профиль, дБ. Пишет в active сразу, если выбран CUSTOM.
void eq_set_custom_gain(int band, float db);
float eq_get_custom_gain(int band);
// Активные усиления (для экрана).
void eq_get_gains(float out_gains[EQ_BANDS]);

// Обработка in-place, int16 interleaved. frames = сэмплов на канал.
// rate = частота трека (44100/48000...), пересчёт при смене.
// Плоская АЧХ = возврат без обработки (бит-перфект).
void eq_process(short *pcm, int frames, int channels, int rate);
int eq_is_active(void);

// Тост после смены: 0 нет, 1 пресет, 2 качество.
int eq_toast_kind(void);
int eq_toast_preset(void);  // пресет при kind==1, иначе -1

#endif
