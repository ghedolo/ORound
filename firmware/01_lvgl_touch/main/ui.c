#include "ui.h"

#include "board_pins.h"
#include "display.h"
#include "lvgl.h"

#define DOT_SIZE 40

static lv_obj_t *s_coord_label;
static lv_obj_t *s_dot;
static lv_obj_t *s_bright_label;

static void screen_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_label_set_text_fmt(s_coord_label, "x %3d  y %3d", (int)p.x, (int)p.y);
        lv_obj_set_pos(s_dot, p.x - DOT_SIZE / 2, p.y - DOT_SIZE / 2);
        lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_RELEASED) {
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    display_set_brightness((uint8_t)value);
    lv_label_set_text_fmt(s_bright_label, "brightness %d", (int)value);
}

static void color_btn_cb(lv_event_t *e)
{
    static const uint32_t colors[] = {0x000000, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
    static int index = 0;
    index = (index + 1) % (sizeof(colors) / sizeof(colors[0]));
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(colors[index]), 0);
}

void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_ALL, NULL);

    // Ring on the panel edge to check alignment and the round shape
    lv_obj_t *ring = lv_arc_create(scr);
    lv_obj_set_size(ring, LCD_H_RES - 4, LCD_V_RES - 4);
    lv_obj_center(ring);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_value(ring, 0);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(ring, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x00A0FF), LV_PART_MAIN);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ORound");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 70);

    s_coord_label = lv_label_create(scr);
    lv_label_set_text(s_coord_label, "touch the screen");
    lv_obj_set_style_text_color(s_coord_label, lv_color_hex(0xC0C0C0), 0);
    lv_obj_align(s_coord_label, LV_ALIGN_TOP_MID, 0, 125);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 160, 56);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn, color_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "color");
    lv_obj_center(btn_label);

    lv_obj_t *slider = lv_slider_create(scr);
    lv_obj_set_width(slider, 260);
    lv_slider_set_range(slider, 10, 255);
    lv_slider_set_value(slider, 255, LV_ANIM_OFF);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -130);
    lv_obj_add_event_cb(slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_bright_label = lv_label_create(scr);
    lv_label_set_text(s_bright_label, "brightness 255");
    lv_obj_set_style_text_font(s_bright_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bright_label, lv_color_hex(0xC0C0C0), 0);
    lv_obj_align_to(s_bright_label, slider, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

    s_dot = lv_obj_create(scr);
    lv_obj_set_size(s_dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0xFF8000), 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING);
}
