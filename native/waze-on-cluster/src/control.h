// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* The control socket: newline-delimited JSON between its owner (the app, or
 * `waze-on-cluster run`) and the tool (README.md "Using it").
 *
 *   owner -> tool  {"cmd":"start","target":"sail"|"map"|"panel"}
 *                  {"cmd":"cluster","on":"0"|"1"}  the map on the cluster off or on
 *                  {"cmd":"hud","on":"0"|"1"}  the map on the HUD off or on
 *                  {"cmd":"hud","hud_width_pct":"0"|"20".."100"}  the HUD map's size
 *                  {"cmd":"hud","hud_x":"-800".."800","hud_y":...}  where it is, from the centre
 *                  {"cmd":"hud","hud_keep_shown":"0"|"1"}  kept shown over BMW's pop-ups, or not
 *                  {"cmd":"guide","hide":0|1}  BMW's turn box hidden or not (guide_setting.h)
 *   tool -> owner  {"ev":"status",...,"hide_bmw_guidance":0|1,"guide_disabled":0|1,
 *                   "cluster":0|1,"hud":0|1,"hud_state":"off"|"disabled"|"idle"|"starting"|"showing"|...,
 *                   "hud_width_pct":...,"hud_x":...,"hud_y":...,"hud_keep_shown":0|1}
 *                  {"ev":"log","line":...}
 *
 * One connection is the owner; a second one is told "busy" and closed. */
#ifndef WOC_CONTROL_H
#define WOC_CONTROL_H

#include <map>
#include <string>

namespace woc {

/* A flat JSON object of string and number values, as the protocol uses. */
bool parse_flat_json(const std::string &line, std::map<std::string, std::string> *out);
std::string json_escape(const std::string &s);

/* content and a newline in the file name beside fifo_path, in the render's
 * own RuntimeDirectory, by rename, mode 0644: status.json and probe.txt,
 * which the remote diagnostics read without taking the owner slot. */
void write_sibling_file(const std::string &fifo_path, const char *name, const std::string &content);

class Control {
public:
    struct Handler {
        virtual ~Handler() {}
        virtual void on_command(const std::map<std::string, std::string> &msg) = 0;
        virtual void on_owner_connected() = 0;
        virtual void on_owner_gone() = 0;
    };

    explicit Control(Handler *h) : handler_(h) {}
    ~Control();

    /* Listen: the systemd-passed socket if there is one, else create path. */
    bool listen(const std::string &path, unsigned mode, const std::string &group, std::string *err);

    /* For poll(): the listening socket and the owner connection (-1 if none). */
    int listen_fd() const { return listen_fd_; }
    int owner_fd() const { return owner_fd_; }

    void on_listen_readable();
    void on_owner_readable();
    void send(const std::string &json_line);

private:
    void drop_owner();

    Handler *handler_;
    int listen_fd_ = -1;
    int owner_fd_ = -1;
    bool own_path_ = false;
    std::string path_;
    std::string inbuf_;
};

}  // namespace woc

#endif
