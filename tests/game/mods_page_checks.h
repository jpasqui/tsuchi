// The mods' entries on the Mods page, the page on its own over the title menu, and ui.open_mods. Included by
// ui_mod_list_test.cpp, whose stand-ins play the native list, the page's opening and the title menu's adapter.
namespace {
std::vector<std::string> heard_entries;
void hear_entry(void*, const TSUCHI_UiEntryChosen* event) {
    heard_entries.push_back(std::to_string(unsigned(event->menu)) + ":" + event->entry);
}
void mods_page_checks(uint64_t host) {
    UiModListRow row{};
    bool handled = false, result = false;
    external.clear(); external_load_result = TSUCHI_OK;   // the mods alone: no external packages fill the list

    // Tsuchi's own entry went to the title menu's adapter, and choosing it opens the page on its own.
    assert(title_entries.size() == 1 && title_entries[0] == "mods=Mods");
    ui::emit_entry_chosen_to(framework, {TSUCHI_MENU_TITLE, "mods"});
    assert(page_opens == 1);
    ui::emit_entry_chosen_to(framework, {TSUCHI_MENU_TITLE, "another"});
    ui::emit_entry_chosen_to(framework, {TSUCHI_MENU_TITLE_OPTIONS, "mods"});
    assert(page_opens == 1);

    // A mod's entry on the page: one for each mod, with the limits of the other menus' entries.
    TSUCHI_Mod* owner = mod_create("leaf"); assert(owner);
    records[1].context = owner;
    TSUCHI_Subscription subscription{};
    assert(ui::table.on_entry_chosen(owner, hear_entry, nullptr, &subscription) == TSUCHI_OK);
    assert(ui::table.hold_menu_entry(owner, TSUCHI_MENU_MODS, "", "Get Mods", "") == TSUCHI_INVALID);
    assert(ui::table.hold_menu_entry(owner, TSUCHI_MENU_MODS, "get", "", "") == TSUCHI_INVALID);
    assert(ui::table.hold_menu_entry(owner, TSUCHI_MENU_MODS, "get", std::string(81, 'x').c_str(), "") == TSUCHI_INVALID);
    assert(ui::table.hold_menu_entry(owner, TSUCHI_MENU_MODS, "get", "Get Mods", "Browse and install mods.") == TSUCHI_OK);
    assert(ui::table.hold_menu_entry(owner, TSUCHI_MENU_MODS, "more", "More", "") == TSUCHI_CAPACITY);
    assert(title_entries.size() == 1);   // none of it reached the title menu
    // The title menu's entries still go to its own adapter.
    assert(ui::table.hold_menu_entry(owner, TSUCHI_MENU_TITLE, "play", "Play", "") == TSUCHI_OK && title_entries.size() == 2);
    assert(ui::table.release_menu_entry(owner, TSUCHI_MENU_TITLE, "play") == TSUCHI_OK && title_entries.size() == 1);

    // It is a row after the mods: its mod's ID, its label, and marked as an entry.
    callbacks.host_ready(host); assert(ui_mod_list_native_hold(host) == TSUCHI_OK);
    const uint32_t mods = callbacks.count() - 1;
    assert(mods >= 3 && callbacks.row(mods, row) && row.button && !row.requested && !row.has_settings);
    assert(!strcmp(row.id, "leaf") && !strcmp(row.name, "Get Mods") && !callbacks.row(mods + 1, row));
    assert(callbacks.row(0, row) && !row.button);

    // Confirm chooses it, and its mod hears of it at the next frame. Details opens nothing.
    assert(callbacks.action(host, mods, 4, false) && heard_entries.empty());
    step(); assert(heard_entries.size() == 1 && heard_entries[0] == "3:get");
    step(); assert(heard_entries.size() == 1);
    assert(callbacks.action(host, mods, 102, false) && !ui_active_screen());

    // Holding it again changes its label.
    assert(ui::table.hold_menu_entry(owner, TSUCHI_MENU_MODS, "get", "Download mods", "") == TSUCHI_OK);
    assert(callbacks.row(mods, row) && !strcmp(row.name, "Download mods"));

    // Released while the page shows, its row stays and chooses nothing; the next page is without it.
    assert(ui::table.release_menu_entry(owner, TSUCHI_MENU_MODS, "get") == TSUCHI_OK);
    assert(ui::table.release_menu_entry(owner, TSUCHI_MENU_MODS, "get") == TSUCHI_NOT_FOUND);
    assert(callbacks.count() == mods + 1 && callbacks.action(host, mods, 4, false));
    step(); assert(heard_entries.size() == 1);
    assert(callbacks.action(host, 0, 5, false) && !active);
    callbacks.host_lost(host);
    callbacks.host_ready(host); assert(ui_mod_list_native_hold(host) == TSUCHI_OK && callbacks.count() == mods);

    // A stopped mod's entry leaves the same way.
    assert(ui::table.hold_menu_entry(owner, TSUCHI_MENU_MODS, "get", "Get Mods", "") == TSUCHI_OK);
    callbacks.host_lost(host); callbacks.host_ready(host);
    assert(ui_mod_list_native_hold(host) == TSUCHI_OK && callbacks.count() == mods + 1);
    mod_stop(owner); records[1].context = nullptr;
    assert(callbacks.action(host, mods, 4, false)); step(); assert(heard_entries.size() == 1);
    callbacks.host_lost(host); callbacks.host_ready(host);
    assert(ui_mod_list_native_hold(host) == TSUCHI_OK && callbacks.count() == mods);
    callbacks.host_lost(host); active = 0;

    // The page on its own: the list shows as soon as its host is ready, and there is no other tab to go to.
    page_alone = true;
    callbacks.host_ready(host); assert(active == host);
    const unsigned calls_before = menu_calls;
    assert(dispatcher(host, 100, handled, result) && handled && result && active == host);
    handled = result = false;
    assert(dispatcher(host, 101, handled, result) && handled && result && active == host);

    // Back with nothing changed: the list stays up, and the game's own Back closes the page at the next
    // input step. It is sent until the menu takes it in, then no more.
    assert(callbacks.action(host, 0, 5, false) && active == host && stock_backs == 0);
    reject_menu_action = true; listener(0); assert(stock_backs == 0 && menu_calls == calls_before + 1);
    reject_menu_action = false; listener(0); assert(stock_backs == 1);
    listener(0); assert(stock_backs == 1);
    callbacks.host_lost(host); active = 0;

    // Back with a change: the question first, then the same way out whichever the answer.
    callbacks.host_ready(host); assert(active == host);
    const bool was_on = request(2);
    const uint64_t asked_before = asked.id;
    assert(callbacks.action(host, 2, was_on ? 2 : 3, !was_on));
    if (asked.id != asked_before) answer(true);   // the review of what the change takes with it
    assert(request(2) != was_on && callbacks.action(host, 2, 5, false));
    listener(0); assert(stock_backs == 1);   // not while the question waits
    answer(false); assert(active == host);
    listener(0); assert(stock_backs == 2);
    callbacks.host_lost(host); active = 0; page_alone = false; message.clear();

    // ui.open_mods: the page on its own when Settings is not up, and the Mods tab when it takes input.
    assert(ui::table.open_mods(framework) == TSUCHI_OK && page_opens == 2 && tab_shows == 0);
    page_open_result = TSUCHI_NOT_READY;
    assert(ui::table.open_mods(framework) == TSUCHI_NOT_READY && page_opens == 3);
    page_open_result = TSUCHI_OK;
    callbacks.host_ready(host); settings_host = host;
    assert(ui::table.open_mods(framework) == TSUCHI_OK && page_opens == 3 && tab_shows == 1 && active == host);
    assert(ui::table.open_mods(framework) == TSUCHI_OK && tab_shows == 1);   // it shows already
    assert(callbacks.action(host, 0, 102, false) && ui_active_screen());      // a mod's details are up
    active = 0; assert(ui::table.open_mods(framework) == TSUCHI_BUSY);
    ui_action(5); step();
    callbacks.host_lost(host); settings_host = 0; active = 0;
}
}
