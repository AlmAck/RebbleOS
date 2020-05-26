/* notification.c
 * An app for displaying notification messages
 * RebbleOS
 *
 * Author: Carson Katri <me@carsonkatri.com>
 *         Barry Carter <barry.carter@gmail.com>
 */

#include "rebbleos.h"
#include "notification.h"
#include "notification_layer.h"
#include "librebble.h"
#include "bitmap_layer.h"
#include "action_bar_layer.h"
#include "platform_res.h"
#include "menu.h"
#include "timeline.h"
#include "notification_manager.h"
#include "rdb.h"

static NotificationLayer* _notif_layer;
static Window* _notif_window;
static MenuLayer *s_menu_layer;
static Window *s_main_window;

/* We store all the notification keys in a list, and lazy-load each
 * notification to display on screen.  The special case is if there are no
 * notifications at all, in which case we display a single "no
 * notifications" menu list item.
 */
static rdb_select_result_list _notif_list;
static int _notif_count = 0;

static void _notif_window_load(Window *window);
static void _notif_window_unload(Window *window);
static void _exit_to_watchface(struct Menu *menu, void *context);
static void _notif_destroy_layer_cb(ClickRecognizerRef _, void *context);

void notif_init(void)
{
    _notif_window = window_create();

    window_set_window_handlers(_notif_window, (WindowHandlers) {
        .load = _notif_window_load,
        .unload = _notif_window_unload,
    });

    window_stack_push(_notif_window, true);
}

static MenuItems* _msg_list_item_selected(const MenuItem *item)
{
    char *app = "RebbleOS";
    char *title = "Message";
    rebble_notification *msg = (rebble_notification *)item->context;

    rebble_attribute *attr = list_elem(list_get_head(&msg->attributes), rebble_attribute, node);

    Layer *layer = window_get_root_layer(s_main_window);
    GRect bounds = layer_get_unobstructed_bounds(layer);
    _notif_layer = notification_layer_create(bounds);
//     Notification *notification = notification_create(app, title, (const char *)attr->data, gbitmap_create_with_resource(RESOURCE_ID_SPEECH_BUBBLE), GColorRed);
    
//     notification_layer_stack_push_notification(_notif_layer, notification);
    notification_layer_configure_click_config(_notif_layer, s_main_window, _notif_destroy_layer_cb);
    layer_add_child(layer, notification_layer_get_layer(_notif_layer));
        
    layer_mark_dirty(layer);
    window_dirty(true);
    
    return NULL;
}

static void _notif_destroy_layer_cb(ClickRecognizerRef _, void *context)
{
    notification_layer_destroy(_notif_layer);
    window_load_click_config(s_main_window);
}

static uint16_t _notif_menu_get_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *context) {
    if (_notif_count == 0)
        return 1;
    else
        return _notif_count;
}

static void _notif_menu_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *context) {
    if (_notif_count == 0) {
        menu_cell_basic_draw(ctx, cell_layer, "No notifications", "Asleep at the switch?", /* icon, GBitmap */ NULL);
        return;
    }
    
    /* Find the noty. */
    int i = 0;
    struct rdb_select_result *res;
    rdb_select_result_foreach(res, &_notif_list) {
        if (i == cell_index->row)
            break;
        
        i++;
    }
    assert(i == cell_index->row);
    void *key = res->result[0];
    
    /* Now that we have the key, go actually fully load the noty itself. */
    struct rdb_database *db = rdb_open(RDB_ID_NOTIFICATION);
    struct rdb_iter it;
    rdb_select_result_list notif_result;
    
    list_init_head(&notif_result);
    
    if (!rdb_iter_start(db, &it)) {
        rdb_close(db);
        goto failed;
    }
    
    struct rdb_selector selectors[] = {
        { offsetof(timeline_item, uuid), FIELD_SIZEOF(timeline_item, uuid), RDB_OP_EQ, key },
        { 0, 0, RDB_OP_RESULT_FULLY_LOAD },
        { }
    };
    
    if (rdb_select(&it, &notif_result, selectors) != 1) {
        rdb_select_free_all(&notif_result);
        rdb_close(db);
        goto failed;
    }
    
    rdb_close(db);
    
    /* Now we have one noty, loaded in memory!  Grab it from the list... */
    void *item_data;
    rdb_select_result_foreach(res, &notif_result) {
        item_data = res->result[0];
    }
    
    /* ... et voila.  Now just time to render it.  Prioritize what we show. */
    rebble_notification *noty = timeline_item_process(item_data);
    rebble_attribute *a;
    const char *sender = NULL;
    const char *subject = NULL;
    const char *message = NULL;
    
    list_foreach(a, &noty->attributes, rebble_attribute, node) {
        switch (a->timeline_attribute.attribute_id) {
        case TimelineAttributeType_Sender:  sender  = (const char *) a->data; break;
        case TimelineAttributeType_Subject: subject = (const char *) a->data; break;
        case TimelineAttributeType_Message: message = (const char *) a->data; break;
        default:
            /* we don't care */
            ;
        }
    }
    
    if (sender  && !strlen(sender )) sender  = NULL;
    if (subject && !strlen(subject)) subject = NULL;
    if (message && !strlen(message)) message = NULL;
    
    const char *title = NULL, *subtitle = NULL;
    if (sender) {
        title = sender;
        if (subject) {
            subtitle = subject;
        } else if (message) {
            subtitle = message;
        }
    } else if (subject) {
        title = subject;
        if (message) {
            subtitle = message;
        }
    } else if (message) {
        title = message;
    } else {
        title = "No title";
    }

    /* XXX: Retrieve an icon from flash by-app. */
    menu_cell_basic_draw(ctx, cell_layer, title, subtitle, NULL);
    
    /* And clean up. */
    timeline_destroy(noty);
    rdb_select_free_all(&notif_result);
    
    return;
    
failed:
    menu_cell_basic_draw(ctx, cell_layer, "Error", "Failed to load", NULL);
}

static int16_t _notif_menu_get_cell_height(struct MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
    return 44; /* hardcoded for basic_draw: can we get another one somewhere? */
}

static void _notif_menu_select_click(struct MenuLayer *menu_layer, MenuIndex *cell_index, void *context) {
}

static void _notif_window_load(Window *window)
{
    s_main_window = window;
    Layer *window_layer = window_get_root_layer(window);
    MenuItems *items;
    
#ifdef PBL_RECT
    s_menu_layer = menu_layer_create(GRect(0, 16, DISPLAY_COLS, DISPLAY_ROWS - 16));
#else
    // Let the menu draw behind the statusbar so it is perfectly centered
    s_menu_layer = menu_layer_create(GRect(0, 0, DISPLAY_COLS, DISPLAY_ROWS));
#endif

    menu_layer_set_click_config_onto_window(s_menu_layer, window);
    menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
        .get_num_rows = _notif_menu_get_num_rows,
        .draw_row = _notif_menu_draw_row,
        .get_cell_height = _notif_menu_get_cell_height,
        .select_click = _notif_menu_select_click,
        /* XXX: override back button, a la https://gist.github.com/sarfata/10574031 ... or just add a back-door API */
    });
    layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));

    /* Load in the keys for all the notifications on the system. */
    list_init_head(&_notif_list);
    
    struct rdb_database *db = rdb_open(RDB_ID_NOTIFICATION);
    struct rdb_iter it;
    if (rdb_iter_start(db, &it)) {
        struct rdb_selector selectors[] = {
            { offsetof(timeline_item, uuid), FIELD_SIZEOF(timeline_item, uuid), RDB_OP_RESULT },
            { }
        };
        _notif_count = rdb_select(&it, &_notif_list, selectors);
        APP_LOG("noty", APP_LOG_LEVEL_INFO, "%d items from select", _notif_count);
    }

    rdb_close(db);
    
    menu_layer_reload_data(s_menu_layer);

    return;
    
}

static void _notif_window_unload(Window *window)
{
    rdb_select_free_all(&_notif_list);
    menu_layer_destroy(s_menu_layer);

    if (_notif_layer)
    {
        notification_layer_destroy(_notif_layer);
        _notif_layer = NULL;
    }
}

void notif_deinit(void)
{
//     notification_window_destroy(notif_window);
    window_destroy(_notif_window);
}

void notif_main(void)
{
    notif_init();
    app_event_loop();
    notif_deinit();
}

static void _exit_to_watchface(struct Menu *menu, void *context)
{
    // Exit to watchface
    appmanager_app_start("Simple");
}
