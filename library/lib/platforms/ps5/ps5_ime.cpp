/*
Copyright 2026 xfangfang

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <borealis/platforms/ps5/ps5_ime.hpp>

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>

namespace brls
{

namespace
{

// The keyboard is a system library that the console only loads when a title
// asks for it, so it is opened the first time a text field is used and the
// functions are called through the addresses the console hands out.
constexpr const char* kImeDialogLibrary = "libSceImeDialog.sprx";
// A console that names its system libraries the older way still has the same
// dialog, so the second name is tried when the first one is not there.
constexpr const char* kImeDialogLibraryAlt = "libSceImeDialog.prx";
constexpr const char* kUserServiceLibrary  = "libSceUserService.sprx";

// The dialog's description of what it should ask for. The layout is the one
// the console's system libraries take: the same struct is used by the PS4
// build of this library, and the console runs PS4 titles through the same
// dialog. The trailing room is zeroed and only there so that a console that
// knows more fields reads zeros rather than whatever follows this struct.
struct ImeDialogSetting
{
    uint32_t userId;
    int32_t type;
    uint64_t supportedLanguages;
    int32_t enterLabel;
    int32_t inputMethod;
    void* filter;
    uint32_t option;
    uint32_t maxTextLength;
    wchar_t* inputTextBuffer;
    float posx;
    float posy;
    int32_t horizontalAlignment;
    int32_t verticalAlignment;
    const wchar_t* placeholder;
    const wchar_t* title;
    int8_t reserved[64];
};

struct ImeDialogResult
{
    int32_t endstatus;
    int8_t reserved[12];
};

// The keyboard layouts the console offers.
constexpr int kTypeDefault = 0;
constexpr int kTypeNumber  = 4;

constexpr int kAlignHorizontalCenter = 1;
constexpr int kAlignVerticalBottom   = 2;

constexpr int kStatusNone    = 0;
constexpr int kStatusRunning = 1;
constexpr int kStatusStopped = 2;

constexpr int kEndStatusOk = 0;

// The system writes UTF-16; the console's wchar_t is two bytes wide, so the
// two are the same thing here.
constexpr size_t kMaxTextLength = 512;
constexpr size_t kMaxTitleLength = 128;

uint16_t inputText[kMaxTextLength + 1];
uint16_t titleText[kMaxTitleLength];
uint16_t placeholderText[kMaxTitleLength];

struct ImeDialogLibrary
{
    using InitFn      = int (*)(const ImeDialogSetting*, void*);
    using GetStatusFn = int (*)();
    using GetResultFn = int (*)(ImeDialogResult*);
    using TermFn      = int (*)();

    void* module      = nullptr;
    InitFn init       = nullptr;
    GetStatusFn status = nullptr;
    GetResultFn result = nullptr;
    TermFn term       = nullptr;
    bool opened       = false;

    bool open()
    {
        if (opened)
            return init != nullptr;

        opened = true;
        module = dlopen(kImeDialogLibrary, RTLD_LAZY);
        if (module == nullptr)
            module = dlopen(kImeDialogLibraryAlt, RTLD_LAZY);
        if (module == nullptr) {
            const char* reason = dlerror();
            Logger::error("the console's keyboard is not available: {}", reason ? reason : kImeDialogLibrary);
            return false;
        }

        init   = reinterpret_cast<InitFn>(dlsym(module, "sceImeDialogInit"));
        status = reinterpret_cast<GetStatusFn>(dlsym(module, "sceImeDialogGetStatus"));
        result = reinterpret_cast<GetResultFn>(dlsym(module, "sceImeDialogGetResult"));
        term   = reinterpret_cast<TermFn>(dlsym(module, "sceImeDialogTerm"));
        if (init == nullptr || status == nullptr || result == nullptr || term == nullptr) {
            Logger::error("the console's keyboard is missing its entry points");
            init = nullptr;
            return false;
        }

        Logger::info("the console's keyboard is available");
        return true;
    }
};

ImeDialogLibrary keyboard;

// The user the keyboard is opened for. Reading it is best effort: without it
// the dialog cannot know which account's languages to show, and the console
// then answers with an error rather than opening.
uint32_t currentUser()
{
    static int32_t user = -1;
    if (user >= 0)
        return (uint32_t)user;

    user = 0;
    void* service = dlopen(kUserServiceLibrary, RTLD_LAZY);
    if (service != nullptr) {
        using GetUserFn = int (*)(int32_t*);
        auto getUser   = reinterpret_cast<GetUserFn>(dlsym(service, "sceUserServiceGetInitialUser"));
        if (getUser != nullptr && getUser(&user) != 0)
            user = 0;
    }
    return (uint32_t)user;
}

void utf8ToUtf16(const std::string& source, uint16_t* destination, size_t capacity)
{
    size_t used = 0;
    for (size_t i = 0; i < source.size() && used + 1 < capacity;) {
        const unsigned char lead = (unsigned char)source[i];
        uint32_t code;
        size_t extra;
        if (lead < 0x80) {
            code  = lead;
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            code  = lead & 0x1F;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            code  = lead & 0x0F;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            code  = lead & 0x07;
            extra = 3;
        } else {
            ++i;
            continue;
        }
        if (i + extra >= source.size())
            break;

        bool complete = true;
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char next = (unsigned char)source[i + k];
            if ((next & 0xC0) != 0x80) {
                complete = false;
                break;
            }
            code = (code << 6) | (next & 0x3F);
        }
        i += extra + 1;
        if (!complete)
            continue;

        if (code >= 0x10000) {
            if (used + 2 >= capacity)
                break;
            code -= 0x10000;
            destination[used++] = (uint16_t)(0xD800 + (code >> 10));
            destination[used++] = (uint16_t)(0xDC00 + (code & 0x3FF));
        } else {
            destination[used++] = (uint16_t)code;
        }
    }
    destination[used] = 0;
}

std::string utf16ToUtf8(const uint16_t* source)
{
    std::string text;
    for (size_t i = 0; source[i]; ++i) {
        uint32_t code = source[i];
        if (code >= 0xD800 && code <= 0xDBFF) {
            const uint16_t low = source[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }

        if (code < 0x80) {
            text += (char)code;
        } else if (code < 0x800) {
            text += (char)(0xC0 | (code >> 6));
            text += (char)(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            text += (char)(0xE0 | (code >> 12));
            text += (char)(0x80 | ((code >> 6) & 0x3F));
            text += (char)(0x80 | (code & 0x3F));
        } else {
            text += (char)(0xF0 | (code >> 18));
            text += (char)(0x80 | ((code >> 12) & 0x3F));
            text += (char)(0x80 | ((code >> 6) & 0x3F));
            text += (char)(0x80 | (code & 0x3F));
        }
    }
    return text;
}

/**
 * Follow the dialog from the main loop. The console draws the keyboard itself,
 * so there is nothing to render here: the loop only asks whether it is still
 * open. Polling from the main loop instead of waiting inside the call keeps the
 * application's own frames going while the player types, and keeps the text
 * field's callback on the thread that owns the interface.
 */
void waitForInput(std::function<void(std::string)> callback)
{
    struct Poll
    {
        int frames = 0;
        bool shown = false;
    };
    auto poll = std::make_shared<Poll>();

    brls::sync([callback, poll]() {
        const int status = keyboard.status();

        if (status == kStatusRunning) {
            poll->shown = true;
            waitForInput(callback);
            return;
        }

        if (status != kStatusStopped && !poll->shown) {
            // Opening the dialog is not instantaneous; give the console a few
            // seconds to put it on screen before calling the attempt a failure.
            if (++poll->frames < 300) {
                waitForInput(callback);
                return;
            }
            Logger::error("the console's keyboard never opened");
            keyboard.term();
            return;
        }

        ImeDialogResult result {};
        const int rc = keyboard.result(&result);
        keyboard.term();

        Logger::info("keyboard closed: rc=0x{:08x} end={}", (uint32_t)rc, (int)result.endstatus);
        if (rc == 0 && result.endstatus == kEndStatusOk)
            callback(utf16ToUtf8(inputText));
    });
}

} // namespace

bool Ps5ImeManager::openSystemDialog(std::function<void(std::string)> callback, int type,
    const std::string& headerText, const std::string& subText, int maxStringLength,
    const std::string& initialText)
{
    if (!keyboard.open())
        return false;

    const size_t limit = (size_t)std::clamp(maxStringLength, 1, (int)kMaxTextLength);

    utf8ToUtf16(initialText, inputText, limit + 1);
    utf8ToUtf16(headerText, titleText, kMaxTitleLength);
    utf8ToUtf16(subText, placeholderText, kMaxTitleLength);

    ImeDialogSetting setting {};
    setting.userId              = currentUser();
    setting.type                = type;
    setting.maxTextLength       = (uint32_t)limit;
    setting.inputTextBuffer     = reinterpret_cast<wchar_t*>(inputText);
    setting.posx                = 1000;
    setting.posy                = 1000;
    setting.horizontalAlignment = kAlignHorizontalCenter;
    setting.verticalAlignment   = kAlignVerticalBottom;
    setting.placeholder         = reinterpret_cast<const wchar_t*>(placeholderText);
    setting.title               = reinterpret_cast<const wchar_t*>(titleText);

    const int rc = keyboard.init(&setting, nullptr);
    if (rc != 0) {
        Logger::error("the console's keyboard did not open: 0x{:08x}", (uint32_t)rc);
        keyboard.term();
        return false;
    }

    waitForInput(std::move(callback));
    return true;
}

bool Ps5ImeManager::openForText(std::function<void(std::string)> f, std::string headerText,
    std::string subText, int maxStringLength, std::string initialText, int kbdDisableBitmask)
{
    if (openSystemDialog(f, kTypeDefault, headerText, subText, maxStringLength, initialText))
        return true;
    return this->fallback.openForText(f, headerText, subText, maxStringLength, initialText, kbdDisableBitmask);
}

bool Ps5ImeManager::openForNumber(std::function<void(long)> f, std::string headerText,
    std::string subText, int maxStringLength, std::string initialText,
    std::string leftButton, std::string rightButton, int kbdDisableBitmask)
{
    auto accept = [f](const std::string& text) {
        if (text.empty())
            return;
        try {
            f(std::stoll(text));
        } catch (const std::exception& e) {
            Logger::error("Could not parse input, did you enter a valid integer? {}", e.what());
        }
    };

    if (openSystemDialog(accept, kTypeNumber, headerText, subText, maxStringLength, initialText))
        return true;
    return this->fallback.openForNumber(f, headerText, subText, maxStringLength, initialText,
        leftButton, rightButton, kbdDisableBitmask);
}

} // namespace brls
