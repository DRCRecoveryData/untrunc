#include "gui.h"

#include <string.h>  // memset
#include <thread>
#include <filesystem>  // Add this include for filesystem operations

namespace fs = std::filesystem;

extern "C" {
#include "libavutil/log.h"
}

#include "../atom.h"
#include "../common.h"
#include "../mp4.h"
using namespace std;

stringstream Gui::buffer_ = stringstream();
uiWindow* Gui::window_;
uiMultilineEntry* Gui::current_text_entry_;
uiEntry* Gui::Repair::entry_ok_;
uiEntry* Gui::Repair::entry_bad_;
uiMultilineEntry* Gui::Repair::text_entry_;
uiLabel* Gui::Repair::label_status_;
uiProgressBar* Gui::Repair::progressbar_;
uiEntry* Gui::Analyze::entry_ok_;
uiMultilineEntry* Gui::Analyze::text_entry_;
uiProgressBar* Gui::Analyze::progressbar_;
thread* Gui::thread_ = nullptr;

void Gui::init() {
    uiInitOptions o;
    memset(&o, 0, sizeof(uiInitOptions));
    if (uiInit(&o) != NULL) abort();

    window_ = newWindow("untrunc-gui", 820, 640, 0);

    build();
    setSpaced(true);

    cout.rdbuf(buffer_.rdbuf());
    cerr.rdbuf(buffer_.rdbuf());
    av_log_set_callback([](void* ptr, int loglevel, const char* msg, va_list vl) {
        if (loglevel > av_log_get_level()) return;

        const uint limit = 256;
        char buffer[limit+1];
        AVClass *avc = ptr ? *(AVClass**)ptr : NULL;
        auto n = snprintf(buffer, limit, "[%s @ %p] ", avc->item_name(ptr), ptr);
        vsnprintf(buffer+n, limit-n, msg, vl);

        cout << buffer << '\n';
    });

    current_text_entry_ = Gui::Repair::text_entry_;
    uiTimer(100, Gui::writeOutput, NULL);

    uiWindowOnClosing(window_, Gui::onClosing, NULL);
}

void Gui::run() {
    uiControlShow(uiControl(window_));
    uiMain();
}

void Gui::build() {
    struct {
        const char* name;
        uiControl* (*f)(void);
    } pages[] = {
        {"Repair", Gui::repairTab},
        {"Settings", Gui::settingsTab},
        {"Analyze", Gui::analyzeTab},
        {"About", Gui::aboutTab},
        {NULL, NULL},
    };

    auto tabs = newTab();
    for (uint i = 0; pages[i].name != NULL; i++)
        uiTabAppend(tabs, pages[i].name, (*(pages[i].f))());

    uiWindowSetChild(window_, uiControl(tabs));
}

// New method to open a directory
void Gui::openDirectory(uiButton* b, void* data) {
    auto dn = uiOpenFolder(window_);
    if (dn) {
        uiEntrySetText(uiEntry(data), dn);
        uiFreeText(dn);
    }
}

// New method to add a directory open button
uiEntry* Gui::addDirectoryOpen(uiBox* parent_box, const char* text) {
    auto h_box = newHorizontalBox();

    auto button = newButton(text);
    auto entry = newEntry();
    uiButtonOnClicked(button, openDirectory, entry);
    uiBoxAppend(h_box, uiControl(button), 0);
    uiBoxAppend(h_box, uiControl(entry), 1);
    uiBoxAppend(parent_box, uiControl(h_box), 1);
    return entry;
}

void Gui::openFile(uiButton* b, void* data) {
    auto fn = uiOpenFile(window_);
    if (fn) {
        uiEntrySetText(uiEntry(data), fn);
        uiFreeText(fn);
    }
}

int Gui::onClosing(uiWindow* w, void* data) {
    if (thread_)  {
        thread_->join();
        delete(thread_);
    }
    uiQuit();
    return 1;
}

int Gui::writeOutput(void* data) {
    auto txt = uiNewAttributedString(buffer_.str().c_str());
    if (uiAttributedStringLen(txt)) {
        uiMultilineEntryAppend(current_text_entry_, uiAttributedStringString(txt));
        buffer_.str("");
    }
    return 1;
}

uiEntry* Gui::addFileOpen(uiBox* parent_box, const char* text) {
    auto h_box = newHorizontalBox();

    auto button = newButton(text);
    auto entry = newEntry();
    uiButtonOnClicked(button, openFile, entry);
    uiBoxAppend(h_box, uiControl(button), 0);
    uiBoxAppend(h_box, uiControl(entry), 1);
    uiBoxAppend(parent_box, uiControl(h_box), 1);
    return entry;
}

// Updated repairTab method
uiControl* Gui::repairTab() {
    auto v_box = newVerticalBox();

    auto h_box1 = newHorizontalBox();
    Repair::entry_ok_ = addFileOpen(h_box1, "reference file");
    Repair::entry_bad_ = addDirectoryOpen(h_box1, "truncated folder");
    uiBoxAppend(v_box, uiControl(h_box1), 0);

    Repair::text_entry_ = uiNewMultilineEntry();
    uiMultilineEntrySetReadOnly(Repair::text_entry_, 1);
    uiBoxAppend(v_box, uiControl(Repair::text_entry_), 1);

    auto h_box2 = newHorizontalBox();
    Repair::label_status_ = uiNewLabel("");
    auto b_repair = newButton("Repair");
    uiButtonOnClicked(b_repair, startRepair, NULL);
    uiBoxAppend(h_box2, uiControl(Repair::label_status_), 1);
    uiBoxAppend(h_box2, uiControl(b_repair), 0);
    uiBoxAppend(v_box, uiControl(h_box2), 0);

    Repair::progressbar_ = uiNewProgressBar();
    uiBoxAppend(v_box, uiControl(Repair::progressbar_), 0);

    return uiControl(v_box);
}

#define CATCH_THEM(R) \
    catch (const char* e) {cerr << e << '\n'; msgBoxError(e, true); R;} \
    catch (string e) {cerr << e << '\n'; msgBoxError(e, true); R;} \
    catch (const std::exception& e) {cerr << e.what() << '\n'; msgBoxError(e.what(), true); R;}

// Updated startRepair method
void Gui::startRepair(uiButton* b, void* data) {
    current_text_entry_ = Repair::text_entry_;
    uiMultilineEntrySetText(current_text_entry_, "");

    string file_ok = uiEntryText(Repair::entry_ok_);
    string folder_bad = uiEntryText(Repair::entry_bad_);
    if (file_ok.empty() || folder_bad.empty()) {
        msgBoxError("Please specify the reference file and the truncated folder!");
        return;
    }

    setDisabled(true);
    Repair::onProgress(0);
    thread_ = new thread([](const string& file_ok, const string& folder_bad) {
        string output_suffix = g_ignore_unknown ? ss("-s", Mp4::step_) : "";

        try {
            for (const auto& entry : fs::directory_iterator(folder_bad)) {
                if (entry.is_regular_file()) {
                    string file_bad = entry.path().string();
                    Mp4 mp4;
                    g_mp4 = &mp4;  // singleton is overkill, this is good enough
                    g_onProgress = Gui::Repair::onProgress;
                    mp4.parseOk(file_ok);

                    mp4.repair(file_bad);
                    chkHiddenWarnings();
                    Repair::onProgress(100);
                    cout << "\ndone!";

                    if (mp4.premature_end_ && mp4.premature_percentage_ < 0.9) {
                        ASYNC(msgBox, "Encountered premature end, please try '-s' in the \"Settings\" Tab.");
                    }
                }
            }
        }
        CATCH_THEM();
        ASYNC(setDisabled, false);
    }, file_ok, folder_bad);
}

void Gui::Repair::onProgress(int percentage) {
    uiProgressBarSetValue(Repair::progressbar_, percentage);
}

void Gui::Repair::onStatus(const string& status) {
    uiLabelSetText(Repair::label_status_, status.c_str());
}

void Gui::Analyze::onProgress(int percentage) {
    uiProgressBarSetValue(Analyze::progressbar_, percentage);
}
