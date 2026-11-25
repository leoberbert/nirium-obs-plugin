/*
Copyright (C) 2021 DEV47APPS, github.com/dev47apps
Copyright (C) 2026 Leonardo Berbert (adb handling rewritten for the Niriun plugin)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "adb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "command.h"
#include "plugin.h"

// ---- helpers declared in command.h -----------------------------------------------------------------------

bool process_check_success(process_t proc, const char *name)
{
    if (proc == PROCESS_NONE)
        return false;
    bool rc = true;
    exit_code_t exit_code;
    if (!cmd_simple_wait(proc, &exit_code)) {
        if (exit_code != NO_EXIT_CODE)
            elog("\"%s\" exit value %" PRIexitcode, name, exit_code);
        else
            elog("\"%s\" exited unexpectedly with %d", name, (int)exit_code);
        rc = false;
    }
    CloseProcessHandle(proc);
    return rc;
}

// Serialises argv to a command line; arguments with spaces are quoted (needed for "C:\Program Files\...").
bool argv_to_string(const char *const *argv, char *buf, size_t bufsize)
{
    size_t idx = 0;
    bool truncated = false;
    if (bufsize == 0)
        return true;
    while (*argv && !truncated) {
        const char *arg = *argv++;
        bool quote = strchr(arg, ' ') != NULL;
        size_t len = strlen(arg) + (quote ? 2 : 0);
        if (idx + len + 2 > bufsize) {
            truncated = true;
            break;
        }
        if (quote)
            buf[idx++] = '"';
        memcpy(&buf[idx], arg, strlen(arg));
        idx += strlen(arg);
        if (quote)
            buf[idx++] = '"';
        buf[idx++] = ' ';
    }
    if (idx > 0)
        idx--;  // drop the trailing space
    buf[idx] = '\0';
    return truncated;
}

void process_print_error(enum process_result err, const char *const argv[])
{
    char buf[512];
    switch (err) {
    case PROCESS_ERROR_GENERIC:
        argv_to_string(argv, buf, sizeof(buf));
        elog("failed to exec: %s", buf);
        break;
    case PROCESS_ERROR_MISSING_BINARY:
        argv_to_string(argv, buf, sizeof(buf));
        elog("command not found: %s", buf);
        break;
    case PROCESS_SUCCESS:
        break;
    }
}

// ---- Adb -------------------------------------------------------------------------------------------------

Adb &Adb::instance()
{
    static Adb a;
    return a;
}

bool Adb::run(const std::string &serial, const std::vector<std::string> &args, std::string *output)
{
    std::vector<const char *> argv;
    argv.push_back(exe_.c_str());
    if (!serial.empty()) {
        argv.push_back("-s");
        argv.push_back(serial.c_str());
    }
    for (const std::string &a : args)
        argv.push_back(a.c_str());
    argv.push_back(NULL);

    char buf[4096];
    buf[0] = 0;
    process_t proc;
    enum process_result r = cmd_execute(argv[0], argv.data(), &proc, output ? buf : NULL, output ? sizeof(buf) : 0);
    if (r != PROCESS_SUCCESS) {
        process_print_error(r, argv.data());
        return false;
    }
    bool ok = process_check_success(proc, args.empty() ? "adb" : args[0].c_str());
    if (output)
        *output = buf;
    return ok;
}

void Adb::locate()
{
    if (located_)
        return;
    located_ = true;
    exe_.clear();

    std::vector<std::string> candidates;
    if (const char *env = getenv("NIRIUN_ADB"))
        candidates.push_back(env);
    candidates.push_back("adb");  // PATH

#ifdef _WIN32
    const char *exe_name = "adb.exe";
    const char *sep = "\\";
#else
    const char *exe_name = "adb";
    const char *sep = "/";
#endif
    for (const char *var : {"ANDROID_HOME", "ANDROID_SDK_ROOT"}) {
        if (const char *root = getenv(var))
            candidates.push_back(std::string(root) + sep + "platform-tools" + sep + exe_name);
    }
#ifdef _WIN32
    if (const char *local = getenv("LOCALAPPDATA"))
        candidates.push_back(std::string(local) + "\\Android\\Sdk\\platform-tools\\adb.exe");
#else
    if (const char *home = getenv("HOME")) {
        candidates.push_back(std::string(home) + "/Android/Sdk/platform-tools/adb");
        candidates.push_back(std::string(home) + "/Library/Android/sdk/platform-tools/adb");
    }
    candidates.push_back("/usr/local/bin/adb");
    candidates.push_back("/opt/homebrew/bin/adb");
    candidates.push_back("/usr/bin/adb");
#endif
    if (char *bundled = obs_module_file(exe_name)) {
        candidates.push_back(bundled);
        bfree(bundled);
    }

    for (const std::string &c : candidates) {
        bool bare = c == "adb";
        if (!bare && !FileExists(c.c_str()))
            continue;
        exe_ = c;
        std::string out;
        if (run("", {"version"}, &out)) {
            ilog("adb found: %s", c.c_str());
            // Start the server now: the first start takes seconds and must not stall a later connect.
            run("", {"start-server"}, NULL);
            return;
        }
        exe_.clear();
    }
    ilog("adb not found: USB entries are disabled (Wi-Fi does not need it)");
}

bool Adb::available()
{
    std::lock_guard<std::mutex> g(lock_);
    locate();
    return !exe_.empty();
}

std::vector<AdbDevice> Adb::devices()
{
    std::vector<AdbDevice> out;
    std::lock_guard<std::mutex> g(lock_);
    locate();
    if (exe_.empty())
        return out;

    std::string text;
    if (!run("", {"devices", "-l"}, &text))
        return out;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '*' || line.rfind("List of", 0) == 0)
            continue;

        // "SERIAL<spaces>device usb:1-1 product:x model:SM_S938B device:y transport_id:2"
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos)
            continue;
        AdbDevice d;
        d.serial = line.substr(0, sp);
        size_t st = line.find_first_not_of(" \t", sp);
        if (st == std::string::npos)
            continue;
        size_t se = line.find_first_of(" \t", st);
        d.state = line.substr(st, se == std::string::npos ? std::string::npos : se - st);

        size_t m = line.find("model:");
        if (m != std::string::npos) {
            size_t me = line.find(' ', m);
            d.model = line.substr(m + 6, me == std::string::npos ? std::string::npos : me - m - 6);
            for (char &c : d.model)
                if (c == '_')
                    c = ' ';
        }
        out.push_back(std::move(d));
    }
    return out;
}

int Adb::forward(const std::string &serial, uint16_t remote_port)
{
    std::lock_guard<std::mutex> g(lock_);
    locate();
    if (exe_.empty())
        return 0;

    char remote[32];
    snprintf(remote, sizeof(remote), "tcp:%u", (unsigned)remote_port);
    std::string out;
    if (!run(serial, {"forward", "tcp:0", remote}, &out))
        return 0;
    int port = atoi(out.c_str());
    if (port <= 0 || port > 65535) {
        elog("adb forward: unexpected output '%s'", out.c_str());
        return 0;
    }
    return port;
}

void Adb::remove_forward(int local_port)
{
    std::lock_guard<std::mutex> g(lock_);
    if (exe_.empty() || local_port <= 0)
        return;
    char local[32];
    snprintf(local, sizeof(local), "tcp:%d", local_port);
    run("", {"forward", "--remove", local}, NULL);
}
