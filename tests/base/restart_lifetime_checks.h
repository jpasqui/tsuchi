// Exercise the real manager's group preflight, with actual owned resources.
void restart_lifetime_checks() {
    set_before_start(nullptr);
    for (const char* value : {"false", "null", "0", "[]", "{}", "\"never\"", "\"game\\u0000\""}) {
        const std::string declaration = std::string(",\"restart\":") + value;
        install("invalid", declaration.c_str());
        CHECK(find_mods() == TSUCHI_OK);
        CHECK(find_mod("invalid")->state == ModState::blocked);
        CHECK(strstr(problem_of("invalid"), "\"restart\" is either"));
        reset();
    }
    for (const char* request : {"provider", "session"}) {
        install("provider", R"(,"early2":{})");
        install("session", R"(,"restart":"game","requires":{"provider":"*"},"early2":{})");
        install("consumer", R"(,"requires":{"session":"*"},"early2":{})");
        install("independent", R"(,"restart":"mod","early2":{})");
        CHECK(find_mods() == TSUCHI_OK);
        prepare_mods(); remember_contexts(); start_mods();
        CHECK(find_mod("session")->game_restart && !find_mod("provider")->game_restart);
        auto* independent = find_mod("independent")->context;
        find_mod(request)->restart_asked = true;
        find_mod("independent")->restart_asked = true;
        run_restarts();
        // A rejected group must not release a provider, the session mod, or its
        // consumer. A distinct ordinary request still restarts in this step.
        for (const char* id : {"provider", "session", "consumer"}) {
            CHECK(find_mod(id)->context == expected_contexts[id]);
            CHECK(mod_usable(find_mod(id)->context) && owned_count(find_mod(id)->context) == 1);
            CHECK(released[id] == 0 && !find_mod(id)->restart_asked);
        }
        for (const char* id : {"session", "consumer"}) {
            CHECK(restart_outcomes[id].first == TSUCHI_RESTART_OUTCOME_KEPT);
            CHECK(restart_outcomes[id].second == "session requires restarting the game");
        }
        if (!strcmp(request, "provider")) CHECK(restart_outcomes["provider"].first == TSUCHI_RESTART_OUTCOME_KEPT);
        CHECK(!mod_running(independent) && released["independent"] == 1);
        CHECK(find_mod("independent")->state == ModState::running);
        CHECK(restart_outcomes["independent"].first == TSUCHI_RESTART_OUTCOME_RESTARTED);
        // A lost entry file cannot bypass the session policy; the policy belongs
        // to the admitted manifest/context, not to what's currently on disk.
        files.erase(std::string(kModsFolder) + "/session/mod.json");
        find_mod("session")->restart_asked = true;
        run_restarts();
        CHECK(find_mod("session")->context == expected_contexts["session"] && released["session"] == 0);
        reset();
    }
    // Failed startup still rolls back. It can be retried because no session-long
    // allocation was admitted successfully; the flag must not strand ownership.
    install("failed", R"(,"restart":"game","early2":"fail")");
    CHECK(find_mods() == TSUCHI_OK); prepare_mods(); remember_contexts(); start_mods();
    CHECK(find_mod("failed")->state == ModState::blocked && !find_mod("failed")->context);
    CHECK(released["failed"] == 1);
    find_mod("failed")->restart_asked = true;
    run_restarts();
    CHECK(restart_outcomes["failed"].first == TSUCHI_RESTART_OUTCOME_BLOCKED);
    CHECK(!find_mod("failed")->context && released["failed"] == 2);
    reset();
}
