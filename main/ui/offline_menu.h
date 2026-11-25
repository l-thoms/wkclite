#pragma once

#include "ui_common.h"

// Creates the GPIO0 offline menu page (shown as a popup)
ui_page_t *ui_offline_menu_create();
// Pops the GPIO0 offline menu up on top of the current page
void ui_offline_menu_show(ui_shell_t *shell);
