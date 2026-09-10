#ifndef EEZ_LVGL_UI_IMAGES_H
#define EEZ_LVGL_UI_IMAGES_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_bat_40;
extern const lv_img_dsc_t img_bat_60;
extern const lv_img_dsc_t img_bat_80;
extern const lv_img_dsc_t img_bat_100;
extern const lv_img_dsc_t img_bat_20;
extern const lv_img_dsc_t img_main;
extern const lv_img_dsc_t img_wifi2;
extern const lv_img_dsc_t img_wifi3;
extern const lv_img_dsc_t img_wifi4;
extern const lv_img_dsc_t img_wifi1;

#ifndef EXT_IMG_DESC_T
#define EXT_IMG_DESC_T
typedef struct _ext_img_desc_t {
    const char *name;
    const lv_img_dsc_t *img_dsc;
} ext_img_desc_t;
#endif

extern const ext_img_desc_t images[10];


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_IMAGES_H*/