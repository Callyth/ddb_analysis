/*
 *  analysis - Analysis plugin for the DeaDBeeF audio player
 *  Copyright (C) 2025 Kaliban <Callyth@users.noreply.github.com>
 *
 *  This library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License version 3
 *  as published by the Free Software Foundation.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this library.  If not, see <https://www.gnu.org/licenses/>.
 */

#define DDB_API_LEVEL 18
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>

#include <unistd.h>
#include <gtk/gtk.h>
#include <essentia/algorithmfactory.h>
#include <essentia/essentia.h>
#include <essentia/essentiamath.h>
#include <vector>

#include <deadbeef/deadbeef.h>
#include <deadbeef/gtkui_api.h>

using namespace std;

static DB_functions_t *deadbeef = NULL;
static DB_misc_t plugin;
static ddb_gtkui_t *gtkui_plugin = NULL;

struct plugin_config_t
{
    string ChordsDetection_chromaPick;
    bool chords_follow_the_rhythm;
    float ChordsDetection_windowSize;
    int chords_frame_size;
    int chords_hop_size;
    bool chords_enable;

    string RhythmExtractor2013_method;
    bool bpm_enable;
    int bpm_averaging;
    float circle_attenuration_speed;

    bool key_enable;

    int update_fps;
    int strength_length;
} config;

struct bpmResult
{
    bool success = false;
    const char *uri;
    int bpm = 0;
    vector<float> ticks;
    vector<float> estimates;
    vector<float> bpmIntervals;
    float confidence = 0.0f;
    plugin_config_t config;
    string error;
};

struct keyResult
{
    bool success = false;
    const char *uri;
    string key;
    string scale;
    float strength = 0.0f;
    string error;
};

struct chordsResult
{
    float delay;
    bool success = false;
    bool is_follow_the_rhythm;
    const char *uri;
    vector<string> chords;
    vector<float> strength;
    string error;
};

typedef struct
{
    ddb_gtkui_widget_t base; // tihs must be placed at the top

    float chord_delay;
    vector<string> chords;
    vector<float> chords_strength;
    bool is_follow_the_rhythm;

    int bpm = 0;
    vector<float> bpm_ticks;
    vector<float> bpm_estimates;
    vector<float> bpm_intervals;
    float bpm_confidence = 0.0f;
    int bpm_tick_index;
    bool is_multifeature_mode;

    string key;
    string scale;
    float key_strength = 0.0f;

    GtkWidget *bpm_label;
    GtkWidget *key_label;
    GtkWidget *chord_label;
    GtkWidget *bpm_widget;
    GtkWidget *hbox;
    GtkWidget *popup;
    GtkWidget *popup_item;
    GtkWidget *popup_item2;
    GtkWidget *visualizer;
    const char *uri = NULL;
    const char *last_uri = NULL;
    string bpm_text;
    string key_text;
    string chord_text;
    std::mutex chordMutex;
    std::mutex bpmMutex;
    std::mutex keyMutex;
    bool chord_finish = false;
    bool bpm_finish = false;
    bool key_finish = false;
    bool chord_success = false;
    bool bpm_success = false;
    bool key_success = false;
    bool is_config_changed = false;

    float circle_brightness = 1.0f;
} w_analysis_t;

static w_analysis_t *w = nullptr;

gboolean update_label(gpointer user_data)
{
    lock_guard<mutex> bpmlock(w->bpmMutex);
    lock_guard<mutex> chordlock(w->chordMutex);
    lock_guard<mutex> keylock(w->keyMutex);
    gtk_label_set_text(GTK_LABEL(w->bpm_label), w->bpm_text.c_str());
    gtk_label_set_text(GTK_LABEL(w->key_label), w->key_text.c_str());
    gtk_label_set_text(GTK_LABEL(w->chord_label), w->chord_text.c_str());
    return FALSE;
}

gboolean analysis_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
    w_analysis_t *w = (w_analysis_t *)user_data;
    if (event->type == GDK_BUTTON_PRESS && event->button == 3)
    {
        gtk_menu_popup_at_pointer(GTK_MENU(w->popup), (GdkEvent *)event);
        return TRUE;
    }
    return FALSE;
}

void config_response(GtkDialog *dialog, gint response_id, gpointer user_data)
{
    GtkWidget *analysis_properties = (GtkWidget *)user_data;
    GtkWidget *bpm_method = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "bpm_method"));
    GtkWidget *chords_chromaPick = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "chords_chromaPick"));
    GtkWidget *update_fps = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "update_fps"));
    GtkWidget *circle_attenuration_speed = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "circle_attenuration_speed"));
    GtkWidget *chords_windowSize = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "chords_windowSize"));
    GtkWidget *chords_follow_the_rhythm = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "chords_follow_the_rhythm"));
    GtkWidget *enable_bpm = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "enable_bpm"));
    GtkWidget *enable_key = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "enable_key"));
    GtkWidget *enable_chords = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "enable_chords"));
    GtkWidget *bpm_averaging = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "bpm_averaging"));
    GtkWidget *chords_frame_size = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "chords_frame_size"));
    GtkWidget *chords_hop_size = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "chords_hop_size"));
    GtkWidget *strength_length = GTK_WIDGET(g_object_get_data(G_OBJECT(analysis_properties), "strength_length"));

    if (response_id == GTK_RESPONSE_APPLY || response_id == GTK_RESPONSE_OK)
    {
        config.RhythmExtractor2013_method = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(bpm_method));
        config.ChordsDetection_chromaPick = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(chords_chromaPick));
        config.update_fps = gtk_spin_button_get_value(GTK_SPIN_BUTTON(update_fps));
        config.circle_attenuration_speed = gtk_spin_button_get_value(GTK_SPIN_BUTTON(circle_attenuration_speed));
        config.ChordsDetection_windowSize = gtk_spin_button_get_value(GTK_SPIN_BUTTON(chords_windowSize));
        config.bpm_averaging = gtk_spin_button_get_value(GTK_SPIN_BUTTON(bpm_averaging));
        config.chords_frame_size = gtk_spin_button_get_value(GTK_SPIN_BUTTON(chords_frame_size));
        config.chords_hop_size = gtk_spin_button_get_value(GTK_SPIN_BUTTON(chords_hop_size));
        config.strength_length = gtk_spin_button_get_value(GTK_SPIN_BUTTON(strength_length));
        config.bpm_enable = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enable_bpm));
        config.key_enable = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enable_key));
        config.chords_enable = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enable_chords));

        if (config.bpm_enable)
        {
            config.chords_follow_the_rhythm = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chords_follow_the_rhythm));
        }
        else
        {
            config.chords_follow_the_rhythm = false;
        }
        w->is_config_changed = true;
    }
    if (response_id == GTK_RESPONSE_CANCEL || response_id == GTK_RESPONSE_OK)
    {
        gtk_widget_destroy(GTK_WIDGET(dialog));
    }
}

static void analysis_config(GtkMenuItem *menuitem, gpointer user_data)
{
    GtkWidget *analysis_properties = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(analysis_properties), "Analysis Properties");
    gtk_window_set_type_hint(GTK_WINDOW(analysis_properties), GDK_WINDOW_TYPE_HINT_DIALOG);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(analysis_properties));

    GtkWidget *hbox1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox1, FALSE, FALSE, 0);
    GtkWidget *hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox2, FALSE, FALSE, 0);
    GtkWidget *hbox17 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox17, FALSE, FALSE, 0);
    GtkWidget *hbox3 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox3, FALSE, FALSE, 0);
    GtkWidget *hbox4 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox4, FALSE, FALSE, 0);
    GtkWidget *hbox5 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox5, FALSE, FALSE, 0);
    GtkWidget *hbox6 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox6, FALSE, FALSE, 0);
    GtkWidget *hbox7 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox7, FALSE, FALSE, 0);
    GtkWidget *hbox8 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox8, FALSE, FALSE, 0);
    GtkWidget *hbox9 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox9, FALSE, FALSE, 0);
    GtkWidget *hbox10 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox10, FALSE, FALSE, 0);
    GtkWidget *hbox11 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox11, FALSE, FALSE, 0);
    GtkWidget *hbox12 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox12, FALSE, FALSE, 0);
    GtkWidget *hbox13 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox13, FALSE, FALSE, 0);
    GtkWidget *hbox14 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox14, FALSE, FALSE, 0);
    GtkWidget *hbox15 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox15, FALSE, FALSE, 0);
    GtkWidget *hbox16 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content_area), hbox16, FALSE, FALSE, 0);

    GtkWidget *general_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(general_label), "<b>GENERAL</b>");
    gtk_container_add(GTK_CONTAINER(hbox1), general_label);

    GtkWidget *strength_length_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(strength_length_label), "strength length:");
    gtk_container_add(GTK_CONTAINER(hbox17), strength_length_label);

    GtkWidget *strength_length = gtk_spin_button_new_with_range(0, 10, 1);
    gtk_container_add(GTK_CONTAINER(hbox17), strength_length);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(strength_length), config.strength_length);
    g_object_set_data(G_OBJECT(analysis_properties), "strength_length", strength_length);

    GtkWidget *update_fps_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(update_fps_label), "update fps:");
    gtk_container_add(GTK_CONTAINER(hbox2), update_fps_label);

    GtkWidget *update_fps = gtk_spin_button_new_with_range(10, 120, 1);
    gtk_container_add(GTK_CONTAINER(hbox2), update_fps);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(update_fps), config.update_fps);
    g_object_set_data(G_OBJECT(analysis_properties), "update_fps", update_fps);

    GtkWidget *bpm_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(bpm_label), "<b>BPM</b>");
    gtk_container_add(GTK_CONTAINER(hbox3), bpm_label);

    GtkWidget *enable_bpm = gtk_check_button_new_with_label("enable BPM extractor");
    gtk_container_add(GTK_CONTAINER(hbox4), enable_bpm);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enable_bpm), config.bpm_enable);
    g_object_set_data(G_OBJECT(analysis_properties), "enable_bpm", enable_bpm);

    GtkWidget *method_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(method_label), "method:");
    gtk_container_add(GTK_CONTAINER(hbox5), method_label);

    GtkWidget *bpm_method = gtk_combo_box_text_new();
    gtk_container_add(GTK_CONTAINER(hbox5), bpm_method);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bpm_method), "multifeature");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bpm_method), "degara");
    if (config.RhythmExtractor2013_method == "multifeature")
        gtk_combo_box_set_active(GTK_COMBO_BOX(bpm_method), 0);
    else
        gtk_combo_box_set_active(GTK_COMBO_BOX(bpm_method), 1);
    g_object_set_data(G_OBJECT(analysis_properties), "bpm_method", bpm_method);

    GtkWidget *circle_attenuration_speed_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(circle_attenuration_speed_label), "circle attenuration speed:");
    gtk_container_add(GTK_CONTAINER(hbox6), circle_attenuration_speed_label);

    GtkWidget *circle_attenuration_speed = gtk_spin_button_new_with_range(0, 2, 0.05);
    gtk_container_add(GTK_CONTAINER(hbox6), circle_attenuration_speed);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(circle_attenuration_speed), config.circle_attenuration_speed);
    g_object_set_data(G_OBJECT(analysis_properties), "circle_attenuration_speed", circle_attenuration_speed);

    GtkWidget *averging_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(averging_label), "averging:");
    gtk_container_add(GTK_CONTAINER(hbox7), averging_label);

    GtkWidget *bpm_averaging = gtk_spin_button_new_with_range(1, 100, 1);
    gtk_container_add(GTK_CONTAINER(hbox7), bpm_averaging);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(bpm_averaging), config.bpm_averaging);
    g_object_set_data(G_OBJECT(analysis_properties), "bpm_averaging", bpm_averaging);

    GtkWidget *key_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(key_label), "<b>KEY</b>");
    gtk_container_add(GTK_CONTAINER(hbox8), key_label);

    GtkWidget *enable_key = gtk_check_button_new_with_label("enable key detection");
    gtk_container_add(GTK_CONTAINER(hbox9), enable_key);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enable_key), config.key_enable);
    g_object_set_data(G_OBJECT(analysis_properties), "enable_key", enable_key);

    GtkWidget *chords_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(chords_label), "<b>CHORDS</b>");
    gtk_container_add(GTK_CONTAINER(hbox10), chords_label);

    GtkWidget *enable_chords = gtk_check_button_new_with_label("enable chords detection");
    gtk_container_add(GTK_CONTAINER(hbox11), enable_chords);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enable_chords), config.chords_enable);
    g_object_set_data(G_OBJECT(analysis_properties), "enable_chords", enable_chords);

    GtkWidget *chromaPick_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(chromaPick_label), "chromaPick:");
    gtk_container_add(GTK_CONTAINER(hbox12), chromaPick_label);

    GtkWidget *chords_chromaPick = gtk_combo_box_text_new();
    gtk_container_add(GTK_CONTAINER(hbox12), chords_chromaPick);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(chords_chromaPick), "starting_beat");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(chords_chromaPick), "interbeat_median");
    if (config.ChordsDetection_chromaPick == "starting_beat")
        gtk_combo_box_set_active(GTK_COMBO_BOX(chords_chromaPick), 0);
    else
        gtk_combo_box_set_active(GTK_COMBO_BOX(chords_chromaPick), 1);
    g_object_set_data(G_OBJECT(analysis_properties), "chords_chromaPick", chords_chromaPick);

    GtkWidget *windowSize_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(windowSize_label), "windowSize:");
    gtk_container_add(GTK_CONTAINER(hbox13), windowSize_label);

    GtkWidget *chords_windowSize = gtk_spin_button_new_with_range(0, 100, 0.1);
    gtk_container_add(GTK_CONTAINER(hbox13), chords_windowSize);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(chords_windowSize), config.ChordsDetection_windowSize);
    g_object_set_data(G_OBJECT(analysis_properties), "chords_windowSize", chords_windowSize);

    GtkWidget *frame_size_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(frame_size_label), "frame size:");
    gtk_container_add(GTK_CONTAINER(hbox14), frame_size_label);

    GtkWidget *chords_frame_size = gtk_spin_button_new_with_range(1024, 16384, 1024);
    gtk_container_add(GTK_CONTAINER(hbox14), chords_frame_size);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(chords_frame_size), config.chords_frame_size);
    g_object_set_data(G_OBJECT(analysis_properties), "chords_frame_size", chords_frame_size);

    GtkWidget *hop_size_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hop_size_label), "hop size:");
    gtk_container_add(GTK_CONTAINER(hbox15), hop_size_label);

    GtkWidget *chords_hop_size = gtk_spin_button_new_with_range(1024, 8192, 1024);
    gtk_container_add(GTK_CONTAINER(hbox15), chords_hop_size);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(chords_hop_size), config.chords_hop_size);
    g_object_set_data(G_OBJECT(analysis_properties), "chords_hop_size", chords_hop_size);

    GtkWidget *chords_follow_the_rhythm = gtk_check_button_new_with_label("follow the rhythm");
    gtk_container_add(GTK_CONTAINER(hbox16), chords_follow_the_rhythm);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chords_follow_the_rhythm), config.chords_follow_the_rhythm);
    g_object_set_data(G_OBJECT(analysis_properties), "chords_follow_the_rhythm", chords_follow_the_rhythm);

    GtkWidget *apply = gtk_button_new_with_label("apply");
    gtk_dialog_add_action_widget(GTK_DIALOG(analysis_properties), apply, GTK_RESPONSE_APPLY);
    gtk_widget_set_can_default(apply, TRUE);

    GtkWidget *cancel = gtk_button_new_with_label("cancel");
    gtk_dialog_add_action_widget(GTK_DIALOG(analysis_properties), cancel, GTK_RESPONSE_CANCEL);
    gtk_widget_set_can_default(cancel, TRUE);

    GtkWidget *ok = gtk_button_new_with_label("ok");
    gtk_dialog_add_action_widget(GTK_DIALOG(analysis_properties), ok, GTK_RESPONSE_OK);
    gtk_widget_set_can_default(ok, TRUE);

    gtk_widget_show_all(analysis_properties);

    g_signal_connect(analysis_properties, "response", G_CALLBACK(config_response), analysis_properties);

    gtk_dialog_run(GTK_DIALOG(analysis_properties));
}

gboolean analysis_update_display(gpointer user_data)
{
    w_analysis_t *w = (w_analysis_t *)user_data;

    float t = deadbeef->streamer_get_playpos();

    if (config.bpm_enable)
    {
        gtk_widget_show(w->bpm_widget);
        if (w->bpm_finish)
        {
            lock_guard<mutex> lock(w->bpmMutex);
            if (w->bpm_success)
            {

                w->circle_brightness -= (1.0f / config.update_fps) / (w->bpm_intervals[w->bpm_tick_index] * config.circle_attenuration_speed);
                if (w->circle_brightness < 0)
                {
                    w->circle_brightness = 0;
                }

                while (w->bpm_tick_index + 1 < w->bpm_ticks.size() && w->bpm_ticks[w->bpm_tick_index + 1] - t <= 1.0f / config.update_fps)
                {
                    w->circle_brightness = 1.0f;
                    w->bpm_tick_index++;
                }

                gtk_widget_queue_draw(w->visualizer);

                float bpm_current = 0.0f;
                int count = 0;
                for (int i = w->bpm_tick_index - config.bpm_averaging; i <= w->bpm_tick_index; i++)
                {
                    if (i < 0)
                        continue;
                    count++;
                    bpm_current += w->bpm_intervals[i];
                    if (i == w->bpm_tick_index)
                    {
                        bpm_current = bpm_current / count;
                        bpm_current = 60 / bpm_current;
                    }
                }

                if (w->is_multifeature_mode)
                {
                    w->bpm_text = to_string(w->bpm) + "(" + to_string((int)bpm_current) + ") BPM(" + to_string(w->bpm_confidence).substr(0, config.strength_length) + ")";
                }
                else
                {

                    w->bpm_text = to_string(w->bpm) + "(" + to_string((int)bpm_current) + ") BPM";
                }
            }
            else
            {

                w->bpm_text = "BPM error!";
                w->bpm_finish = false;
            }
        }
    }
    else
    {
        gtk_widget_hide(w->bpm_widget);
    }
    if (config.key_enable)
    {
        gtk_widget_show(w->key_label);
        if (w->key_finish)
        {
            lock_guard<mutex> lock(w->keyMutex);
            if (w->key_success)
            {

                w->key_text = w->key + " " + w->scale + "(" + to_string(w->key_strength).substr(0, config.strength_length) + ")";
            }
            else
            {

                w->key_text = "Key error!";
            }
            w->key_finish = false;
        }
    }
    else
    {
        gtk_widget_hide(w->key_label);
    }
    if (config.chords_enable)
    {
        gtk_widget_show(w->chord_label);
        if (w->chord_finish)
        {
            lock_guard<mutex> lock(w->chordMutex);
            if (w->chord_success)
            {
                if (w->is_follow_the_rhythm)
                {
                    w->chord_text = w->chords[w->bpm_tick_index] + "(" + to_string(w->chords_strength[w->bpm_tick_index]).substr(0, config.strength_length) + ")";
                }
                else
                {

                    int n = trunc(t / w->chord_delay);

                    if (n >= w->chords.size())
                    {
                        n = w->chords.size() - 1;
                    }
                    w->chord_text = w->chords[n] + "(" + to_string(w->chords_strength[n]).substr(0, config.strength_length) + ")";
                }
            }
            else
            {
                w->chord_text = "Chord error!";
                w->chord_finish = false;
            }
        }
    }
    else
    {
        gtk_widget_hide(w->chord_label);
    }
    g_idle_add(update_label, w);

    if (w->is_config_changed)
    {
        w->is_config_changed = false;
        g_timeout_add((1.0f / config.update_fps) * 1000, analysis_update_display, w);
        return FALSE;
    }
    return TRUE;
}

gboolean draw_circle(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    w_analysis_t *w = (w_analysis_t *)data;

    cairo_set_source_rgba(cr, 1, 1, 1, w->circle_brightness);
    cairo_arc(cr, 15, 15, 12, 0, 2 * M_PI);
    cairo_fill(cr);

    return FALSE;
}

void chords_analysis_worker(const char *path, vector<float> ticks, plugin_config_t config, function<void(chordsResult)> callback)
{
    chordsResult result;
    essentia::standard::Algorithm *loader = nullptr;
    essentia::standard::Algorithm *frameCutter = nullptr;
    essentia::standard::Algorithm *window = nullptr;
    essentia::standard::Algorithm *spectrum = nullptr;
    essentia::standard::Algorithm *peaks = nullptr;
    essentia::standard::Algorithm *hpcp = nullptr;
    essentia::standard::Algorithm *chordsDetection = nullptr;

    try
    {
        essentia::standard::AlgorithmFactory &factory = essentia::standard::AlgorithmFactory::instance();
        vector<essentia::Real> audioBuffer;
        loader = factory.create("MonoLoader", "filename", path, "sampleRate", 44100);
        loader->output("audio").set(audioBuffer);
        loader->compute();

        frameCutter = essentia::standard::AlgorithmFactory::create("FrameCutter", "frameSize", config.chords_frame_size, "hopSize", config.chords_hop_size);
        std::vector<essentia::Real> frame;
        frameCutter->input("signal").set(audioBuffer);
        frameCutter->output("frame").set(frame);

        window = essentia::standard::AlgorithmFactory::create("Windowing", "type", "blackmanharris92"); // option(?)
        std::vector<essentia::Real> windowed;
        window->output("frame").set(windowed);

        spectrum = essentia::standard::AlgorithmFactory::create("Spectrum");
        std::vector<essentia::Real> spec;
        spectrum->output("spectrum").set(spec);

        peaks = essentia::standard::AlgorithmFactory::create("SpectralPeaks");
        std::vector<essentia::Real> freqs, mags;
        peaks->output("frequencies").set(freqs);
        peaks->output("magnitudes").set(mags);

        hpcp = factory.create(
            "HPCP",
            "size", 12,     // should be 12 ?
            "harmonics", 4, // 4~8 ?
            "weightType", "squaredCosine",
            "bandPreset", true,
            "normalized", "unitMax", // unitSum ?
            "nonLinear", true);

        std::vector<essentia::Real>
            hpcpOut;
        hpcp->output("hpcp").set(hpcpOut);
        std::vector<std::vector<essentia::Real>> allHPCPs;

        int count = 0;
        while (true)
        {
            count++;
            frameCutter->compute();
            if (frame.empty())
            {
                break;
            }

            window->input("frame").set(frame);
            window->compute();

            spectrum->input("frame").set(windowed);
            spectrum->compute();

            peaks->input("spectrum").set(spec);
            peaks->compute();

            hpcp->input("frequencies").set(freqs);
            hpcp->input("magnitudes").set(mags);
            hpcp->compute();

            allHPCPs.push_back(hpcpOut);
        }

        vector<string> chordName;
        vector<essentia::Real> chordStrength;

        if (ticks.size() != 0)
        {
            chordsDetection = essentia::standard::AlgorithmFactory::create("ChordsDetectionBeats",
                                                                           "chromaPick", config.ChordsDetection_chromaPick,
                                                                           "hopSize", config.chords_hop_size);
            chordsDetection->input("ticks").set(ticks);
            result.is_follow_the_rhythm = true;
        }
        else
        {
            chordsDetection = essentia::standard::AlgorithmFactory::create("ChordsDetection", "windowSize", config.ChordsDetection_windowSize,
                                                                           "hopSize", config.chords_hop_size);
            result.delay = config.chords_hop_size / 44100.0;
            result.is_follow_the_rhythm = false;
        }
        chordsDetection->input("pcp").set(allHPCPs);
        chordsDetection->output("chords").set(chordName);
        chordsDetection->output("strength").set(chordStrength);

        chordsDetection->compute();

        result.success = true;
        result.chords = chordName;
        result.strength = (vector<float>)chordStrength;
        result.uri = path;

        delete loader;
        delete frameCutter;
        delete window;
        delete spectrum;
        delete peaks;
        // delete whitening;
        delete hpcp;
        delete chordsDetection;
    }
    catch (exception &e)
    {
        result.success = false;
        result.error = e.what();
        if (loader)
        {
            delete loader;
        }
        if (frameCutter)
        {
            delete frameCutter;
        }
        if (window)
        {
            delete window;
        }
        if (spectrum)
        {
            delete spectrum;
        }
        if (peaks)
        {
            delete peaks;
        }
        // if (whitening)
        // {
        //      delete whitening;
        // }
        if (hpcp)
        {
            delete hpcp;
        }
        if (chordsDetection)
        {
            delete chordsDetection;
        }
    }
    callback(result);
}

void key_analysis_worker(const char *path, function<void(keyResult)> callback)
{
    keyResult result;
    essentia::standard::Algorithm *loader = nullptr;
    essentia::standard::Algorithm *keyExtractor = nullptr;

    try
    {
        essentia::standard::AlgorithmFactory &factory = essentia::standard::AlgorithmFactory::instance();
        vector<essentia::Real> audioBuffer;
        loader = factory.create("MonoLoader", "filename", path, "sampleRate", 44100);
        loader->output("audio").set(audioBuffer);
        loader->compute();

        keyExtractor = factory.create("KeyExtractor");

        string key, scale;
        essentia::Real strength;

        keyExtractor->input("audio").set(audioBuffer);
        keyExtractor->output("key").set(key);
        keyExtractor->output("scale").set(scale);
        keyExtractor->output("strength").set(strength);

        keyExtractor->compute();

        result.success = true;
        result.key = key;
        result.scale = scale;
        result.strength = strength;
        result.uri = path;
        delete loader;
        delete keyExtractor;
    }
    catch (exception &e)
    {
        result.success = false;
        result.error = e.what();
        if (loader)
        {
            delete loader;
        }
        if (keyExtractor)
        {
            delete keyExtractor;
        }
    }

    callback(result);
}

void bpm_analysis_worker(const char *path, plugin_config_t config, function<void(bpmResult)> callback)
{
    bpmResult result;
    essentia::standard::Algorithm *loader = nullptr;
    essentia::standard::Algorithm *rhythm = nullptr;

    try
    {

        essentia::standard::AlgorithmFactory &factory = essentia::standard::AlgorithmFactory::instance();
        vector<essentia::Real> audioBuffer;
        loader = factory.create("MonoLoader", "filename", path, "sampleRate", 44100);
        loader->output("audio").set(audioBuffer);
        loader->compute();

        rhythm = factory.create("RhythmExtractor2013", "method", config.RhythmExtractor2013_method);

        essentia::Real bpmValue, confidence;
        vector<essentia::Real> ticks, estimates, bpmIntervals;

        rhythm->input("signal").set(audioBuffer);
        rhythm->output("bpm").set(bpmValue);
        rhythm->output("ticks").set(ticks);
        rhythm->output("confidence").set(confidence);
        rhythm->output("estimates").set(estimates);
        rhythm->output("bpmIntervals").set(bpmIntervals);

        rhythm->compute();

        result.config = config;
        result.success = true;
        result.bpm = trunc(bpmValue);
        result.confidence = (float)confidence;
        result.bpmIntervals = (vector<float>)bpmIntervals;
        result.estimates = (vector<float>)estimates;
        result.ticks = (vector<float>)ticks;
        result.uri = path;
        delete loader;
        delete rhythm;
    }
    catch (exception &e)
    {
        result.success = false;
        result.error = e.what();
        if (loader)
        {
            delete loader;
        }
        if (rhythm)
        {
            delete rhythm;
        }
    }

    callback(result);
}

void chords_callback(chordsResult r)
{
    if (strcmp(r.uri, w->last_uri) == 0)
    {
        if (r.success == true)
        {
            std::lock_guard<std::mutex> lock(w->chordMutex);
            w->chords = r.chords;
            w->chord_delay = r.delay;
            w->chords_strength = r.strength;
            w->is_follow_the_rhythm = r.is_follow_the_rhythm;
            w->chord_success = true;
            w->chord_finish = true;
        }
        else
        {
            std::lock_guard<std::mutex> lock(w->chordMutex);
            w->chord_success = false;
            w->chord_finish = true;
            deadbeef->log("Chord error: %s\n", r.error.c_str());
        }
    }
}

void bpm_callback(bpmResult r)
{
    if (strcmp(r.uri, w->last_uri) == 0)
    {
        if (r.success == true)
        {
            std::lock_guard<std::mutex> lock(w->bpmMutex);
            w->bpm = r.bpm;
            w->bpm_confidence = r.confidence;
            w->bpm_estimates = r.estimates;
            w->bpm_intervals = r.bpmIntervals;
            w->bpm_ticks = r.ticks;
            w->bpm_success = true;
            if (r.config.RhythmExtractor2013_method == "multifeature")
            {
                w->is_multifeature_mode = true;
            }
            else
            {
                w->is_multifeature_mode = false;
            }

            w->bpm_finish = true;

            if (r.config.chords_enable && r.config.chords_follow_the_rhythm)
            {
                w->chord_text = "Calculating...";

                g_idle_add(update_label, w);
                thread chords_worker(chords_analysis_worker, r.uri, w->bpm_ticks, config, chords_callback);
                chords_worker.detach();
            }
        }
        else
        {
            std::lock_guard<std::mutex> lock(w->bpmMutex);
            w->bpm_success = false;
            w->bpm_finish = true;
            deadbeef->log("BPM error: %s\n", r.error.c_str());
        }
    }
}

void key_callback(keyResult r)
{
    if (strcmp(r.uri, w->last_uri) == 0)
    {
        if (r.success == true)
        {
            std::lock_guard<std::mutex> lock(w->keyMutex);
            w->key = r.key;
            w->scale = r.scale;
            w->key_strength = r.strength;
            w->key_success = true;
            w->key_finish = true;
        }
        else
        {
            std::lock_guard<std::mutex> lock(w->keyMutex);
            w->key_success = false;
            w->key_finish = true;
            deadbeef->log("Key error: %s\n", r.error.c_str());
        }
    }
}

static void calculating_music()
{
    lock_guard<mutex> chordlock(w->chordMutex);
    lock_guard<mutex> keylock(w->keyMutex);
    w->chord_finish = false;
    w->bpm_finish = false;
    w->key_finish = false;
    w->chord_success = false;
    w->bpm_success = false;
    w->key_success = false;
    w->last_uri = w->uri;

    if (config.bpm_enable)
    {
        w->bpm_text = "Calculating...";
        thread bpm_worker(bpm_analysis_worker, w->uri, config, bpm_callback);
        bpm_worker.detach();
    }
    else
    {
        w->bpm_text = "...";
    }
    if (config.key_enable)
    {
        w->key_text = "Calculating...";
        thread key_worker(key_analysis_worker, w->uri, key_callback);
        key_worker.detach();
    }
    else
    {
        w->key_text = "...";
    }
    if (config.chords_enable && !config.chords_follow_the_rhythm)
    {
        w->chord_text = "Calculating...";
        vector<float> ticks;
        ticks.clear();
        thread chords_worker(chords_analysis_worker, w->uri, ticks, config, chords_callback);
        chords_worker.detach();
    }
    else if (config.chords_enable)
    {
        w->chord_text = "Waiting...";
    }
    else
    {
        w->chord_text = "...";
    }
    g_idle_add(update_label, w);
}

void analysis_init_gui(ddb_gtkui_widget_t *s)
{
    GtkStyleContext *ctx = gtk_widget_get_style_context(w->base.widget);
    gtk_style_context_add_class(ctx, "panel");

    gtk_style_context_add_class(
        gtk_widget_get_style_context(w->base.widget),
        "background");

    w->bpm_text = "";
    w->key_text = "";
    w->chord_text = "";

    w->bpm_label = gtk_label_new(w->bpm_text.c_str());
    w->key_label = gtk_label_new(w->key_text.c_str());
    w->chord_label = gtk_label_new(w->chord_text.c_str());
    w->visualizer = gtk_drawing_area_new();
    gtk_widget_set_size_request(w->visualizer, 30, 30);

    w->popup = gtk_menu_new();
    w->popup_item = gtk_menu_item_new_with_mnemonic("Configure");
    w->popup_item2 = gtk_menu_item_new_with_mnemonic("Recalculate");

    w->bpm_widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    w->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(w->hbox), w->bpm_widget, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(w->bpm_widget), w->visualizer, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(w->bpm_widget), w->bpm_label, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(w->hbox), w->key_label, FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(w->hbox), w->chord_label, FALSE, FALSE, 5);

    gtk_label_set_line_wrap(GTK_LABEL(w->bpm_label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(w->bpm_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_line_wrap(GTK_LABEL(w->key_label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(w->key_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_line_wrap(GTK_LABEL(w->chord_label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(w->chord_label), PANGO_WRAP_WORD_CHAR);

    gtk_widget_set_halign(w->hbox, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(w->bpm_widget, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(w->key_label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(w->chord_label, GTK_ALIGN_CENTER);

    gtk_container_add(GTK_CONTAINER(w->base.widget), w->hbox);

    gtk_menu_shell_append(GTK_MENU_SHELL(w->popup), w->popup_item);
    gtk_widget_show(w->popup_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(w->popup), w->popup_item2);
    gtk_widget_show(w->popup_item2);
    gtk_widget_show_all(w->base.widget);

    gtk_widget_add_events(w->base.widget, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(w->base.widget, "button-press-event", G_CALLBACK(analysis_button_press), w);
    g_signal_connect_after(GTK_WIDGET(w->popup_item), "activate", G_CALLBACK(analysis_config), w);
    g_signal_connect_after(GTK_WIDGET(w->popup_item2), "activate", G_CALLBACK(calculating_music), w);
    g_signal_connect(w->visualizer, "draw", G_CALLBACK(draw_circle), w);

    g_timeout_add((1.0f / config.update_fps) * 1000, analysis_update_display, w);
}

void w_analysis_init(ddb_gtkui_widget_t *s)
{
    analysis_init_gui(s);
}

void w_analysis_destroy(ddb_gtkui_widget_t *w)
{
}

static void check_url_update()
{
    ddb_playItem_t *track = deadbeef->streamer_get_playing_track();

    if (track)
    {
        w->uri = deadbeef->pl_find_meta(track, ":URI");
        if (w->uri)
        {
            lock_guard<mutex> bpmlock(w->bpmMutex);
            w->bpm_tick_index = 0;
            w->circle_brightness = 1.0f;
            if (!w->last_uri || strcmp(w->uri, w->last_uri) != 0)
            {
                calculating_music();
            }
        }
    }
}

static int analysis_message(ddb_gtkui_widget_t *widget, uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2)
{
    check_url_update();
    return 0;
}

ddb_gtkui_widget_t *w_analysis_create()
{
    w = new w_analysis_t();
    if (!w)
    {
        deadbeef->log("OMG");
        return NULL;
    }

    w->is_follow_the_rhythm = config.chords_follow_the_rhythm;

    w->base.widget = gtk_event_box_new();
    w->base.init = w_analysis_init;
    w->base.destroy = w_analysis_destroy;
    w->base.message = analysis_message;
    gtkui_plugin->w_override_signals(w->base.widget, w);

    return (ddb_gtkui_widget_t *)w;
}

static int plugin_start()
{
    return 0;
}

static int plugin_stop()
{
    return 0;
}

void get_config()
{
    config.RhythmExtractor2013_method = (string)deadbeef->conf_get_str_fast("analysis.bpm_method", "degara");
    config.ChordsDetection_chromaPick = (string)deadbeef->conf_get_str_fast("analysis.chords_chromaPick", "interbeat_median");
    config.update_fps = deadbeef->conf_get_int("analysis.update_fps", 60);
    config.strength_length = deadbeef->conf_get_int("analysis.strength_length", 4);
    config.chords_frame_size = deadbeef->conf_get_int("analysis.chords_frame_size", 8192);
    config.chords_hop_size = deadbeef->conf_get_int("analysis.chords_hop_size", 1024);
    config.bpm_averaging = deadbeef->conf_get_int("bpm_averaging", 15);
    config.circle_attenuration_speed = deadbeef->conf_get_float("analysis.circle_attenuration_speed", 0.75);
    config.ChordsDetection_windowSize = deadbeef->conf_get_float("analysis.ChordsDetection_windowSize", 1.8);
    config.chords_follow_the_rhythm = (bool)deadbeef->conf_get_int("analysis.chords_follow_the_rhythm", 0);
    config.chords_enable = (bool)deadbeef->conf_get_int("analysis.chords_enable", 1);
    config.key_enable = (bool)deadbeef->conf_get_int("analysis.key_enable", 1);
    config.bpm_enable = (bool)deadbeef->conf_get_int("analysis.bpm_enable", 1);
}

void set_config()
{
    deadbeef->conf_set_str("analysis.bpm_method", config.RhythmExtractor2013_method.c_str());
    deadbeef->conf_set_str("analysis.chords_chromaPick", config.ChordsDetection_chromaPick.c_str());
    deadbeef->conf_set_int("analysis.update_fps", config.update_fps);
    deadbeef->conf_set_int("analysis.strength_length", config.strength_length);
    deadbeef->conf_set_int("analysis.chords_frame_size", config.chords_frame_size);
    deadbeef->conf_set_int("analysis.chords_hop_size", config.chords_hop_size);
    deadbeef->conf_set_int("bpm_averaging", config.bpm_averaging);
    deadbeef->conf_set_float("analysis.circle_attenuration_speed", config.circle_attenuration_speed);
    deadbeef->conf_set_float("analysis.ChordsDetection_windowSize", config.ChordsDetection_windowSize);
    deadbeef->conf_set_int("analysis.chords_follow_the_rhythm", (int)config.chords_follow_the_rhythm);
    deadbeef->conf_set_int("analysis.chords_enable", (int)config.chords_enable);
    deadbeef->conf_set_int("analysis.key_enable", (int)config.key_enable);
    deadbeef->conf_set_int("analysis.bpm_enable", (int)config.bpm_enable);
}

static int plugin_connect()
{
    get_config();
    essentia::init();
    gtkui_plugin = (ddb_gtkui_t *)deadbeef->plug_get_for_id(DDB_GTKUI_PLUGIN_ID);
    if (gtkui_plugin)
    {
        if (gtkui_plugin->gui.plugin.version_major == 2)
        {
            gtkui_plugin->w_reg_widget("Analysis", 0, w_analysis_create, "Analysis", NULL);
            check_url_update();
            return 0;
        }
    }
    return -1;
}

static int plugin_disconnect()
{
    set_config();
    essentia::shutdown();
    gtkui_plugin = NULL;
    return 0;
}

void init_plugin()
{
    plugin.plugin.type = DB_PLUGIN_MISC;
    plugin.plugin.id = "analysis";
    plugin.plugin.name = "Analysis";
    plugin.plugin.descr = "Analysis with essentia";
    plugin.plugin.copyright = "analysis - Analysis plugin for the DeaDBeeF audio player\n"
                              "Copyright (C) 2025 Kaliban <Callyth@users.noreply.github.com>\n"
                              "\n"
                              "This library is free software: you can redistribute it and/or modify\n"
                              "it under the terms of the GNU Affero General Public License version 3\n"
                              "as published by the Free Software Foundation.\n"
                              "\n"
                              "This library is distributed in the hope that it will be useful,\n"
                              "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
                              "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n"
                              "\n"
                              "You should have received a copy of the GNU Affero General Public License\n"
                              "along with this library.  If not, see <https://www.gnu.org/licenses/>.\n";
    plugin.plugin.website = "https://github.com/Callyth/ddb_analysis";
    plugin.plugin.api_vmajor = 1;
    plugin.plugin.api_vminor = 18;
    plugin.plugin.version_major = 0;
    plugin.plugin.version_minor = 5;
    plugin.plugin.start = plugin_start;
    plugin.plugin.stop = plugin_stop;
    plugin.plugin.connect = plugin_connect;
    plugin.plugin.disconnect = plugin_disconnect;
}

extern "C" DB_plugin_t *ddb_analysis_GTK3_load(DB_functions_t *ddb)
{
    init_plugin();
    deadbeef = ddb;
    return &plugin.plugin;
}
