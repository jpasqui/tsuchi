// The existing made-up system loader starts the fixture's compiled C after its main.js.
namespace {
void javascript_mods() {
    const auto other_files = disk.files;
    const std::string mods_at = std::string(tsuchi::kModsFolder) + "/";
    tsuchi::forget_mods(); disk.files.clear(); logged.lines.clear(); tsuchi::set_log_sink(log_sink);
    for (const CheckModFile* f = javascript_mod_files; f->mod; ++f)
        if (!std::strcmp(f->mod, "check.javascript")) disk.files[mods_at + f->mod + "/" + f->path] = std::string(f->bytes, f->size);
    disk.files[mods_at + "check.library/mod.json"] = R"({"id":"check.library","name":"Library","version":"1.4.2"})";
    CompiledStart start = nullptr;
    for (const CheckModStart* f = javascript_mod_starts; f->mod; ++f) if (!std::strcmp(f->mod,"check.javascript")) start = f->start;
    CHECK(start != nullptr);
    const int which = int(made_loader.code.size()); made_loader.code.push_back(start);
    disk.files[mods_at + "check.javascript/mod.nro"] = module_image(which);
    // Host expectation comes from the builder, never from the script under test.
    for (const CheckModFile* f = javascript_mod_files; f->mod; ++f)
        if (!std::strcmp(f->mod,"check.javascript") && !std::strcmp(f->path,"language.txt")) disk.files[mods_at+"check.javascript/language.txt"] = std::string(f->bytes,f->size);
    const uint32_t before = tsuchi_js::runtime_count();
    CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods();
    auto* record = tsuchi::find_mod("check.javascript");
    if (!record || record->state != tsuchi::ModState::running) std::fprintf(stderr,"javascript startup: %s\n",record ? record->problem.get() : "missing");
    CHECK(record && record->state == tsuchi::ModState::running && tsuchi_js::runtime_count() == before+1);
    CHECK(logged.lines.find("[check.javascript] started check.javascript 1.0.0 with 1.4.2") < logged.lines.find("[check.javascript] compiled started"));
    if (logged.lines.find("language same") == std::string::npos) std::fprintf(stderr,"javascript log: %s\n",logged.lines.c_str());
    CHECK(logged.lines.find("[check.javascript] helper 42") != std::string::npos && logged.lines.find("[check.javascript] language same") != std::string::npos);
    logged.lines.clear();
    for (int i=0;i<30;++i) tsuchi::frame(0.03f);
    CHECK(logged.lines.find("[check.javascript] frames 30") < logged.lines.find("[check.javascript] compiled frames 30"));
    tsuchi::forget_mods(); CHECK(tsuchi_js::runtime_count() == before);
    logged.lines.clear(); tsuchi::frame(0.03f); CHECK(logged.lines.find("[check.javascript]") == std::string::npos);
    // A mixed-script package is refused before either runtime or the compiled code starts.
    disk.files[mods_at+"check.javascript/main.lua"] = "print('lua ran')";
    CHECK(tsuchi::find_mods() == TSUCHI_OK); tsuchi::start_mods(); record=tsuchi::find_mod("check.javascript");
    CHECK(record && record->state==tsuchi::ModState::blocked && !record->context && tsuchi_js::runtime_count()==before);
    CHECK(logged.lines.find("compiled started")==std::string::npos && logged.lines.find("lua ran")==std::string::npos);
    tsuchi::forget_mods(); disk.files.erase(mods_at+"check.javascript/main.lua");
    // Failure after a subscription unwinds the same registry as Lua and leaves no runtime behind.
    disk.files[mods_at+"check.javascript/main.js"]="time.on('step',()=>print('should not run')); throw new Error('on purpose');";
    CHECK(tsuchi::find_mods()==TSUCHI_OK); tsuchi::start_mods(); record=tsuchi::find_mod("check.javascript");
    CHECK(record && record->state==tsuchi::ModState::blocked && record->problem.get()[0] && tsuchi_js::runtime_count()==before);
    logged.lines.clear(); tsuchi::frame(0.03f); CHECK(logged.lines.find("should not run")==std::string::npos);
    tsuchi::forget_mods();
    // A runtime waiting on a dialog ends before the game delivers its answer.
    disk.files[mods_at+"check.javascript/main.js"]="const answer=await dialog.confirm('Waiting'); print('resumed after stop');";
    disk.files.erase(mods_at+"check.javascript/mod.nro");
    const size_t pending_start = talk.posted.size();
    const std::string talk_log = talk.log;
    const std::vector<std::string> talk_answers = talk.answers;
    CHECK(tsuchi::find_mods()==TSUCHI_OK); tsuchi::start_mods();
    CHECK(tsuchi_js::runtime_count()==before+1); tsuchi::forget_mods(); CHECK(tsuchi_js::runtime_count()==before);
    logged.lines.clear(); tsuchi::frame(0.03f); CHECK(logged.lines.find("resumed after stop")==std::string::npos);
    for (size_t i = pending_start; i < talk.posted.size(); ++i) game_dialog_dispose(reinterpret_cast<void*>(talk.posted[i]));
    tsuchi::frame(0.03f);
    // The game's queue is as it was, so later checks see only the dialogs they ask for.
    talk.posted.resize(pending_start), talk.log = talk_log, talk.answers = talk_answers;
    disk.files=other_files; tsuchi::set_log_sink(nullptr);
}
}