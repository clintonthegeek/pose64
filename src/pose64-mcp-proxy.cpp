/* -*- mode: C++; tab-width: 4 -*- */
/**
 * pose64-mcp-proxy.cpp — MCP-to-ReControl TCP bridge
 *
 * Bridges JSON-RPC (MCP protocol over stdio) to the POSE64 emulator's
 * ReControl text-command TCP server.
 *
 *   Claude Code <--stdin/stdout JSON-RPC--> pose64-mcp-proxy <--TCP--> emulator:6416
 *
 * Single-threaded.  No Qt, no httplib, no threads.  POSIX sockets + nlohmann/json.
 * Target: C++17.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <iostream>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "json.hpp"

using json = nlohmann::json;

// ============================================================================
// Globals
// ============================================================================

static std::string g_host = "127.0.0.1";
static int         g_port = 6416;
static int         g_sock = -1;
static int         g_recv_timeout_sec = 60;  // SO_RCVTIMEO; must exceed the
                                             // server's slowest command (a 4MB
                                             // install ~47s). Tunable via
                                             // --recv-timeout (mainly for tests).

// recv outcome, so callers can distinguish "server wedged" from "peer closed".
enum RecvStatus { RECV_OK = 1, RECV_LOST = 0, RECV_TIMEOUT = -1 };


// ============================================================================
// Logging (all to stderr — stdout is clean JSON-RPC only)
// ============================================================================

static void log_msg (const std::string& msg)
{
    fprintf (stderr, "[pose64-mcp-proxy] %s\n", msg.c_str ());
}

// ============================================================================
// TCP helpers
// ============================================================================

static int tcp_connect (const std::string& host, int port)
{
    int fd = socket (AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        log_msg ("socket() failed: " + std::string (strerror (errno)));
        return -1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons (port);

    if (inet_pton (AF_INET, host.c_str (), &addr.sin_addr) <= 0)
    {
        log_msg ("inet_pton() failed for host: " + host);
        close (fd);
        return -1;
    }

    if (connect (fd, (struct sockaddr*) &addr, sizeof (addr)) < 0)
    {
        log_msg ("connect() failed: " + std::string (strerror (errno)));
        close (fd);
        return -1;
    }

    // Per-command read timeout: a wedged ReControl server then surfaces as a
    // structured timeout instead of hanging this (and every subsequent) call.
    struct timeval tv;
    tv.tv_sec  = g_recv_timeout_sec;
    tv.tv_usec = 0;
    setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));

    return fd;
}

static bool tcp_send (int fd, const std::string& msg)
{
    size_t total = 0;
    while (total < msg.size ())
    {
        ssize_t n = send (fd, msg.data () + total, msg.size () - total, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        total += (size_t) n;
    }
    return true;
}

// Read until '\n'.  Returns a RecvStatus so callers can tell a wedged server
// (RECV_TIMEOUT, from SO_RCVTIMEO) apart from a closed connection (RECV_LOST).
// The line is returned WITHOUT the trailing newline.
static int tcp_recv_line (int fd, std::string& out)
{
    out.clear ();
    char ch;
    while (true)
    {
        ssize_t n = recv (fd, &ch, 1, 0);
        if (n == 0)
            return RECV_LOST;                       // peer closed (EOF)
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return RECV_TIMEOUT;                 // SO_RCVTIMEO expired
            return RECV_LOST;                        // other socket error
        }
        if (ch == '\n')
            return RECV_OK;
        out += ch;
    }
}

// Read multiple lines until a line containing only ".".
// Returns true on success.  Result includes all lines (including the first
// OK line) joined by newline, but excludes the terminating "." line.
static bool tcp_recv_multiline (int fd, std::string& result)
{
    result.clear ();
    std::string line;
    while (true)
    {
        if (tcp_recv_line (fd, line) != RECV_OK)
            return false;
        if (line == ".")
            return true;
        if (!result.empty ())
            result += '\n';
        result += line;
    }
}

// ============================================================================
// Reconnect logic
// ============================================================================

static bool tcp_reconnect ()
{
    if (g_sock >= 0)
    {
        close (g_sock);
        g_sock = -1;
    }
    log_msg ("reconnecting to " + g_host + ":" + std::to_string (g_port));
    g_sock = tcp_connect (g_host, g_port);
    return g_sock >= 0;
}

// Ensure we have a TCP connection.  Returns true if connected.
static bool tcp_ensure_connected ()
{
    if (g_sock >= 0)
        return true;
    log_msg ("connecting to " + g_host + ":" + std::to_string (g_port));
    g_sock = tcp_connect (g_host, g_port);
    if (g_sock >= 0)
        log_msg ("connected");
    return g_sock >= 0;
}

// Send a command and receive a single-line response.
//
// Reconnect policy (landmine #4): a reconnect+resend is only safe when the
// command did NOT reach the server (the send itself failed).  Once the command
// has been delivered, we never blindly resend it — a dropped *response* must not
// double-execute a non-idempotent command (install/key/type/poke/delete/...).
static std::string rc_command (const std::string& cmd, bool idempotent = false)
{
    std::string full = cmd + "\n";
    std::string resp;

    if (!tcp_ensure_connected ())
        return "ERR transient: cannot reach ReControl (is pose64 running?)";

    // Send failed => command never reached the server => safe to reconnect+resend.
    if (!tcp_send (g_sock, full))
    {
        if (!tcp_reconnect () || !tcp_send (g_sock, full))
            return "ERR transient: ReControl connection lost (command not sent)";
    }

    // Command delivered.  Interpret the receive outcome without blind resends.
    int rc = tcp_recv_line (g_sock, resp);
    if (rc == RECV_OK)
        return resp;
    if (rc == RECV_TIMEOUT)
        return "ERR timeout: no response from ReControl within "
               + std::to_string (g_recv_timeout_sec)
               + "s; the command may still be running — query state before retrying.";
    // RECV_LOST: connection dropped after delivery.  Resend only if a re-run is
    // side-effect-free; otherwise report honestly instead of double-executing.
    if (idempotent)
    {
        if (tcp_reconnect () && tcp_send (g_sock, full)
            && tcp_recv_line (g_sock, resp) == RECV_OK)
            return resp;
        return "ERR transient: ReControl connection lost";
    }
    return "ERR transient: ReControl connection lost after the command was sent; "
           "it may or may not have executed — query state/apps before retrying.";
}

// Send a command and receive a multi-line response, with one reconnect attempt.
// Multi-line commands (ui, info, apps) return "OK ...\n" first line, then more lines
// terminated by ".\n".  But if the command fails, ReControl returns "ERR ...\n"
// as a single line (no dot terminator).  So we read the first line, and only
// continue to multi-line read if it starts with "OK".
static std::string rc_command_multi (const std::string& cmd, bool idempotent = false)
{
    std::string full = cmd + "\n";
    std::string first_line;

    if (!tcp_ensure_connected ())
        return "ERR transient: cannot reach ReControl (is pose64 running?)";

    if (!tcp_send (g_sock, full))
    {
        if (!tcp_reconnect () || !tcp_send (g_sock, full))
            return "ERR transient: ReControl connection lost (command not sent)";
    }

    int rc = tcp_recv_line (g_sock, first_line);
    if (rc == RECV_TIMEOUT)
        return "ERR timeout: no response from ReControl within "
               + std::to_string (g_recv_timeout_sec) + "s.";
    if (rc == RECV_LOST)
    {
        // Resend only when the caller marked this command idempotent.
        if (!idempotent || !tcp_reconnect () || !tcp_send (g_sock, full)
            || tcp_recv_line (g_sock, first_line) != RECV_OK)
            return "ERR transient: ReControl connection lost";
    }

    // If first line is ERR, return it immediately (no dot terminator follows)
    if (first_line.substr (0, 3) == "ERR")
        return first_line;

    // Read remaining lines until "."
    std::string rest;
    if (!tcp_recv_multiline (g_sock, rest))
        return "ERR transient: ReControl connection lost during multi-line read";

    if (rest.empty ())
        return first_line;
    return first_line + "\n" + rest;
}

// ============================================================================
// JSON-RPC response helpers
// ============================================================================

// Convert a Latin-1 (ISO-8859-1) string to valid UTF-8 so nlohmann/json
// won't throw type_error.316 when serialising text from the emulator.
// ASCII bytes (0x00-0x7F) pass through unchanged; bytes 0x80-0xFF are
// encoded as two-byte UTF-8 sequences (U+0080 through U+00FF).
static std::string latin1_to_utf8 (const std::string& s)
{
    std::string out;
    out.reserve (s.size ());
    for (unsigned char c : s)
    {
        if (c < 0x80)
        {
            out += (char) c;
        }
        else
        {
            // Two-byte UTF-8: 110xxxxx 10xxxxxx
            out += (char) (0xC0 | (c >> 6));
            out += (char) (0x80 | (c & 0x3F));
        }
    }
    return out;
}

static json make_result (const json& id, const json& result)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

static json make_error (const json& id, int code, const std::string& message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

static json make_tool_result (const json& id, const std::string& text, bool is_error = false)
{
    json content = json::array ();
    content.push_back ({{"type", "text"}, {"text", latin1_to_utf8 (text)}});
    json result = {{"content", content}};
    if (is_error)
        result["isError"] = true;
    return make_result (id, result);
}

static json make_tool_image (const json& id, const std::string& base64data)
{
    json content = json::array ();
    content.push_back ({{"type", "image"}, {"data", base64data}, {"mimeType", "image/png"}});
    return make_result (id, {{"content", content}});
}

static json make_tool_image_text (const json& id, const std::string& base64data,
                                  const std::string& text)
{
    json content = json::array ();
    content.push_back ({{"type", "image"}, {"data", base64data}, {"mimeType", "image/png"}});
    content.push_back ({{"type", "text"}, {"text", latin1_to_utf8 (text)}});
    return make_result (id, {{"content", content}});
}

// ============================================================================
// Base64 encoder (for screenshot PNG)
// ============================================================================

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode (const std::vector<uint8_t>& data)
{
    std::string out;
    size_t len = data.size ();
    out.reserve (((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3)
    {
        uint32_t a = data[i];
        uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
        uint32_t c = (i + 2 < len) ? data[i + 2] : 0;

        uint32_t triple = (a << 16) | (b << 8) | c;

        out += b64_table[(triple >> 18) & 0x3F];
        out += b64_table[(triple >> 12) & 0x3F];
        out += (i + 1 < len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? b64_table[(triple     ) & 0x3F] : '=';
    }

    return out;
}

// ============================================================================
// Tool table — the single source of truth
// ============================================================================
//
// Every MCP tool is ONE entry in kTools[].  tools/list, dispatch, argument
// validation, and reconnect/idempotency policy all derive from it.  (Phase 3
// replaced three parallel structures that had drifted: the tools/list
// builder, the dispatch if-chain, and cmd_is_idempotent().)
//
// Multiline-ness and idempotency are per-ACTION, not per-tool ("break list"
// is multiline+idempotent, "break set" is neither), so each tool's builder
// returns them alongside the TCP command.

static json make_schema (json properties = {}, std::vector<std::string> required = {})
{
    json schema = {{"type", "object"}};
    if (!properties.empty ())
        schema["properties"] = properties;
    if (!required.empty ())
        schema["required"] = json (required);
    return schema;
}

static json int_prop (const std::string& desc)
{
    return {{"type", "integer"}, {"description", desc}};
}

static json str_prop (const std::string& desc)
{
    return {{"type", "string"}, {"description", desc}};
}

static json bool_prop (const std::string& desc)
{
    return {{"type", "boolean"}, {"description", desc}};
}

static json enum_prop (const std::string& desc, std::vector<std::string> values)
{
    json p = {{"type", "string"}, {"description", desc}};
    p["enum"] = json (values);
    return p;
}

struct BuiltCmd
{
    std::string cmd;        // TCP command to send (when err is empty)
    std::string err;        // non-empty => "ERR usage: <err>", nothing sent
    bool multiline  = false;
    bool idempotent = false;
};

static BuiltCmd built (std::string cmd, bool multiline = false, bool idempotent = false)
{
    BuiltCmd b;
    b.cmd = std::move (cmd);
    b.multiline = multiline;
    b.idempotent = idempotent;
    return b;
}

static BuiltCmd usage_err (std::string msg)
{
    BuiltCmd b;
    b.err = std::move (msg);
    return b;
}

using SchemaFn  = json (*) ();
using BuildFn   = BuiltCmd (*) (const json&);
using HandlerFn = json (*) (const json&, const json&);

struct ToolDef
{
    const char* name;
    const char* description;
    SchemaFn    schema;
    BuildFn     build;      // nullptr when custom is set
    HandlerFn   custom;     // nullptr for plain command tools
};

static std::string istr (const json& v)   // validated integer -> string
{
    return std::to_string (v.get<long long> ());
}

static std::string sstr (const json& v)   // validated string -> string
{
    return v.get<std::string> ();
}

// Generic argument validation against the tool's own schema: required
// arguments present, basic types correct, enum membership, no unknown
// arguments.  A failure returns the usage message and NOTHING is sent to
// the emulator — no silent defaults (the old proxy turned a missing 'x'
// into "tap 0 0").
static std::string validate_args (const json& schema, const json& args)
{
    if (!args.is_object ())
        return "arguments must be an object";

    if (schema.contains ("required"))
        for (const auto& r : schema["required"])
            if (!args.contains (r.get<std::string> ()))
                return "missing required argument '" + r.get<std::string> () + "'";

    const json props = schema.value ("properties", json::object ());
    for (auto it = args.begin (); it != args.end (); ++it)
    {
        if (!props.contains (it.key ()))
            return "unknown argument '" + it.key () + "'";
        const json& p = props[it.key ()];
        const std::string type = p.value ("type", "");
        if (type == "integer" && !it.value ().is_number_integer ())
            return "argument '" + it.key () + "' must be an integer";
        if (type == "string" && !it.value ().is_string ())
            return "argument '" + it.key () + "' must be a string";
        if (type == "boolean" && !it.value ().is_boolean ())
            return "argument '" + it.key () + "' must be a boolean";
        if (p.contains ("enum"))
        {
            bool ok = false;
            for (const auto& e : p["enum"])
                if (e == it.value ())
                    ok = true;
            if (!ok)
                return "argument '" + it.key () + "' must be one of " + p["enum"].dump ();
        }
    }
    return "";
}

// ============================================================================
// Custom handlers — tools whose MCP result is more than a command passthrough
// ============================================================================

static json h_ping (const json& id, const json&)
{
    std::string resp = rc_command ("state", true);
    if (resp.rfind ("OK", 0) == 0)
        return make_tool_result (id, "pong");
    return make_tool_result (id, resp, true);
}

static json h_state (const json& id, const json&)
{
    std::string state = rc_command ("state", true);
    std::string info  = rc_command_multi ("info", true);
    return make_tool_result (id, json ({{"state", state}, {"info", info}}).dump (2));
}

static json h_dialog (const json& id, const json& args)
{
    std::string respond = args.value ("respond", std::string ());
    if (respond.empty ())
        return make_tool_result (id, rc_command_multi ("dialog", true));
    std::string resp = rc_command ("dialog respond " + respond);
    return make_tool_result (id, resp, resp.rfind ("OK", 0) != 0);
}

static json h_screenshot (const json& id, const json& args)
{
    std::string path = args.value ("path", std::string ());
    bool has_path = !path.empty ();
    if (!has_path)
        path = "/tmp/pose64_screenshot.png";

    std::string cmd = "screenshot " + path;

    int  scale    = args.value ("scale", 1);
    bool grid     = args.value ("grid", false);
    bool annotate = args.value ("annotate", false);
    std::string crosshair = args.value ("crosshair", std::string ());

    crosshair.erase (std::remove (crosshair.begin (), crosshair.end (), ' '), crosshair.end ());
    crosshair.erase (std::remove (crosshair.begin (), crosshair.end (), '\n'), crosshair.end ());
    if (!crosshair.empty () && crosshair.find (',') == std::string::npos)
        return make_tool_result (id, "ERR usage: crosshair must be 'x,y' (e.g. '80,72')", true);

    if (scale > 1)
        cmd += " scale=" + std::to_string (scale);
    if (grid)
        cmd += " grid";
    if (annotate)
        cmd += " annotate";
    if (!crosshair.empty ())
        cmd += " crosshair=" + crosshair;

    std::string resp = rc_command (cmd);
    if (resp.substr (0, 2) != "OK")
        return make_tool_result (id, resp, true);

    if (has_path)
    {
        if (annotate)
        {
            std::string ui_text = rc_command_multi ("ui", true);
            return make_tool_result (id, resp + "\n" + ui_text);
        }
        return make_tool_result (id, resp);
    }

    std::ifstream file (path, std::ios::binary);
    if (!file)
        return make_tool_result (id, "ERR: could not read " + path, true);

    std::vector<uint8_t> data ((std::istreambuf_iterator<char> (file)),
                                std::istreambuf_iterator<char> ());
    std::string b64 = base64_encode (data);

    if (annotate)
    {
        std::string ui_text = rc_command_multi ("ui", true);
        return make_tool_image_text (id, b64, ui_text);
    }

    return make_tool_image (id, b64);
}

// ============================================================================
// The catalog.  27 core tools (this task) + 10 debug-surface tools (next).
// ============================================================================

static const ToolDef kTools[] = {

{ "palm_ping", "Ping the emulator to check if it is responsive.",
  [] { return make_schema (); },
  nullptr, h_ping },

{ "palm_state", "Get emulator state (running/suspended/blocked_on_ui) and device info as JSON.",
  [] { return make_schema (); },
  nullptr, h_state },

{ "palm_ui", "Read the current Palm OS form/UI structure: object types, IDs, labels, bounds, text.",
  [] { return make_schema (); },
  [] (const json&) { return built ("ui", true, true); }, nullptr },

{ "palm_apps", "List installed applications. Set all=true to list EVERY database "
  "(data, resources, libraries), not just launchable apps.",
  [] { return make_schema ({{"all", bool_prop ("List all databases, not just apps (default false)")}}); },
  [] (const json& a) { return built (a.value ("all", false) ? "apps all" : "apps", true, true); },
  nullptr },

{ "palm_tap", "Tap at screen coordinates (0-159). Returns 'OK delivered' only once the "
  "guest's event queue actually has the event (delivery-honest, blocks <=2s); a refused "
  "or undelivered tap is a truthful ERR, never a silent OK.",
  [] { return make_schema ({{"x", int_prop ("X coordinate (0-159)")},
                            {"y", int_prop ("Y coordinate (0-159)")}}, {"x", "y"}); },
  [] (const json& a) { return built ("tap " + istr (a["x"]) + " " + istr (a["y"])); }, nullptr },

{ "palm_tap_id", "Tap the center of a form object by its numeric ID (from palm_ui). "
  "Delivery-honest like palm_tap.",
  [] { return make_schema ({{"id", int_prop ("Object ID from palm_ui")}}, {"id"}); },
  [] (const json& a) { return built ("tap-id " + istr (a["id"])); }, nullptr },

{ "palm_pen", "Send a single pen down or up event at coordinates. Delivery-honest.",
  [] { return make_schema ({{"action", enum_prop ("Pen action", {"down", "up"})},
                            {"x", int_prop ("X coordinate (0-159)")},
                            {"y", int_prop ("Y coordinate (0-159)")}},
                           {"action", "x", "y"}); },
  [] (const json& a) { return built ("pen " + sstr (a["action"]) + " "
                                     + istr (a["x"]) + " " + istr (a["y"])); }, nullptr },

{ "palm_key", "Send a key event by decimal character code. Delivery-honest.",
  [] { return make_schema ({{"code", int_prop ("Character code (decimal)")}}, {"code"}); },
  [] (const json& a) { return built ("key " + istr (a["code"])); }, nullptr },

{ "palm_type", "Type a string of text (UTF-8 in, converted to Latin-1). Delivery-honest.",
  [] { return make_schema ({{"text", str_prop ("Text to type")}}, {"text"}); },
  [] (const json& a) { return built ("type " + sstr (a["text"])); }, nullptr },

{ "palm_button", "Press a hardware button. Queued contract: OK means enqueued to the "
  "hardware-button state, not delivery-confirmed; ERR busy when gremlin/playback active.",
  [] { return make_schema ({{"name", enum_prop ("Button", {"power", "up", "down", "app1",
                                                           "app2", "app3", "app4",
                                                           "cradle", "contrast"})},
                            {"action", enum_prop ("Action", {"down", "up", "tap"})}},
                           {"name", "action"}); },
  [] (const json& a) { return built ("button " + sstr (a["name"]) + " " + sstr (a["action"])); },
  nullptr },

{ "palm_screenshot", "Take a screenshot. Returns base64 image data if no path given, else "
  "saves PNG to path. scale/grid/annotate/crosshair add AI-friendly coordinate overlays; "
  "the returned CRC is always of raw pre-overlay pixels.",
  [] { return make_schema ({{"path", str_prop ("File path to save PNG (optional; default returns image data)")},
                            {"scale", int_prop ("Integer upscale, e.g. 4 for 640x640 (default 1, max 16)")},
                            {"grid", bool_prop ("Coordinate grid overlay with rulers (default false)")},
                            {"annotate", bool_prop ("UI bounding boxes with IDs; also returns palm_ui text (default false)")},
                            {"crosshair", str_prop ("Mark a point: 'x,y', e.g. '80,72'")}}); },
  nullptr, h_screenshot },

{ "palm_screen_hash", "CRC32 hash of current screen pixels + dimensions (fast change detection).",
  [] { return make_schema (); },
  [] (const json&) { return built ("screen-hash", false, true); }, nullptr },

{ "palm_launch", "Launch an application by database name (names with spaces are fine).",
  [] { return make_schema ({{"app", str_prop ("Application database name (from palm_apps)")}}, {"app"}); },
  [] (const json& a) { return built ("launch " + sstr (a["app"])); }, nullptr },

{ "palm_install", "Install a .prc/.pdb file into the emulator (max 4MB).",
  [] { return make_schema ({{"path", str_prop ("Path to .prc or .pdb file")}}, {"path"}); },
  [] (const json& a) { return built ("install " + sstr (a["path"])); }, nullptr },

{ "palm_export", "Export a database from the emulator to a host .prc/.pdb file.",
  [] { return make_schema ({{"db", str_prop ("Database name (from palm_apps)")},
                            {"path", str_prop ("Host file path to write")}}, {"db", "path"}); },
  [] (const json& a) { return built ("export " + sstr (a["db"]) + " " + sstr (a["path"])); },
  nullptr },

{ "palm_save", "Save the current emulator session to a .psf file.",
  [] { return make_schema ({{"path", str_prop ("Path to save session file")}}, {"path"}); },
  [] (const json& a) { return built ("save " + sstr (a["path"])); }, nullptr },

{ "palm_load", "Load an emulator session from a .psf file (replaces the current session).",
  [] { return make_schema ({{"path", str_prop ("Path to session file")}}, {"path"}); },
  [] (const json& a) { return built ("load " + sstr (a["path"])); }, nullptr },

{ "palm_reset", "Reset the emulated device. Works even in blocked_on_ui (dismisses any dialog).",
  [] { return make_schema ({{"type", enum_prop ("Reset type (default soft)", {"soft", "hard", "debug"})}}); },
  [] (const json& a) { return built (a.contains ("type") ? "reset " + sstr (a["type"]) : "reset"); },
  nullptr },

{ "palm_sleep", "Pause command processing for 1-30000 milliseconds.",
  [] { return make_schema ({{"ms", int_prop ("Milliseconds (1-30000)")}}, {"ms"}); },
  [] (const json& a) { return built ("sleep " + istr (a["ms"])); }, nullptr },

{ "palm_quit", "Quit the emulator process.",
  [] { return make_schema (); },
  [] (const json&) { return built ("quit"); }, nullptr },

{ "palm_dialog", "Query a pending modal dialog (message, buttons, full CPU register dump when "
  "blocked_on_ui), or respond to dismiss it. Omit 'respond' to just query.",
  [] { return make_schema ({{"respond", enum_prop ("Button to click (omit to query)",
                                                   {"ok", "cancel", "continue", "debug",
                                                    "reset", "yes", "no"})}}); },
  nullptr, h_dialog },

{ "palm_run", "Execute a batch of commands in one call, separated by semicolons. "
  "Sub-commands: tap, pen, key, type, button, sleep, repeat N { ... }. Queued contract "
  "(not delivery-confirmed). Example: 'tap 12 148; sleep 150; type x'.",
  [] { return make_schema ({{"script", str_prop ("Semicolon-separated commands")}}, {"script"}); },
  [] (const json& a) { return built ("run " + sstr (a["script"])); }, nullptr },

{ "palm_peek", "Read 1-256 bytes from emulated memory. Address formats: 0x<hex> (absolute), "
  "a5@<offset> (A5-relative), global.<name> (low-memory global). Works in blocked_on_ui.",
  [] { return make_schema ({{"addr", str_prop ("Address (0x<hex>, a5@<offset>, global.<name>)")},
                            {"nbytes", int_prop ("Bytes to read (1-256)")}}, {"addr", "nbytes"}); },
  [] (const json& a) { return built ("peek " + sstr (a["addr"]) + " " + istr (a["nbytes"]),
                                     false, true); }, nullptr },

{ "palm_poke", "Write bytes to emulated memory.",
  [] { return make_schema ({{"addr", str_prop ("Address (0x<hex>, a5@<offset>, global.<name>)")},
                            {"nbytes", int_prop ("Bytes to write (1-256)")},
                            {"data", str_prop ("Hex string of bytes (e.g. '00A1B2C3')")}},
                           {"addr", "nbytes", "data"}); },
  [] (const json& a) { return built ("poke " + sstr (a["addr"]) + " " + istr (a["nbytes"])
                                     + " " + sstr (a["data"])); }, nullptr },

{ "palm_regs", "Read all m68k CPU registers (D0-D7, A0-A7, PC, SR). Works in blocked_on_ui.",
  [] { return make_schema (); },
  [] (const json&) { return built ("regs", false, true); }, nullptr },

{ "palm_menu", "Trigger a menu item by menu title and item title (posts a menuEvent).",
  [] { return make_schema ({{"menu", str_prop ("Menu title (e.g. 'Options')")},
                            {"item", str_prop ("Item title or substring (e.g. 'About')")}},
                           {"menu", "item"}); },
  [] (const json& a) { return built ("menu \"" + sstr (a["menu"]) + "\" \""
                                     + sstr (a["item"]) + "\""); }, nullptr },

{ "palm_delete", "Delete a database from the emulated device.",
  [] { return make_schema ({{"db", str_prop ("Database name to delete")}}, {"db"}); },
  [] (const json& a) { return built ("delete " + sstr (a["db"])); }, nullptr },

// ============================================================================
// Debug-surface tools (Task A5) — 10 entries, completing the 37-tool catalog
// ============================================================================

{ "palm_speed", "Set or query emulation speed. '100' = 1x wall-clock, '200' = 2x, 'max' = "
  "unthrottled (pegs a host core by design — use briefly). Omit value to query.",
  [] { return make_schema ({{"value", str_prop ("Percent 1-10000, or 'max'. Omit to query.")}}); },
  [] (const json& a) -> BuiltCmd {
      if (!a.contains ("value"))
          return built ("speed", false, true);
      std::string v = sstr (a["value"]);
      if (v == "max")
          return built ("speed max");
      char* end = nullptr;
      long pct = strtol (v.c_str (), &end, 10);
      if (end == v.c_str () || *end != '\0' || pct < 1 || pct > 10000)
          return usage_err ("value must be 1-10000 or 'max'");
      return built ("speed " + std::to_string (pct));
  }, nullptr },

{ "palm_backtrace", "Stack crawl of the emulated m68k CPU: PC and A6 per frame. Works in "
  "blocked_on_ui (frozen CPU) — the first tool to reach for after a crash dialog.",
  [] { return make_schema (); },
  [] (const json&) { return built ("backtrace", true, true); }, nullptr },

{ "palm_break", "Manage the 6 m68k breakpoint slots. action=set requires idx+addr (condition "
  "optional, e.g. 'd0 == 0'); clear/enable/disable require idx. On hit the CPU "
  "blocks on a Continue/Debug/Reset dialog (state=blocked_on_ui): inspect with "
  "palm_dialog/palm_backtrace/palm_peek, resume with palm_dialog "
  "respond=continue. With an external SLP debugger attached (--slp-debugger), "
  "the debugger takes the hit instead.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"list", "set", "clear",
                                                                "enable", "disable", "clearall"})},
                            {"idx", int_prop ("Breakpoint slot 0-5")},
                            {"addr", str_prop ("Code address, e.g. '0x10C32A40' (set)")},
                            {"condition", str_prop ("Optional condition (set), e.g. 'd0 == 0'")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "list")
          return built ("break list", true, true);
      if (act == "clearall")
          return built ("break clearall");
      if (act == "set")
      {
          if (!a.contains ("idx") || !a.contains ("addr"))
              return usage_err ("action 'set' requires 'idx' and 'addr'");
          std::string cmd = "break set " + istr (a["idx"]) + " " + sstr (a["addr"]);
          if (a.contains ("condition"))
              cmd += " " + sstr (a["condition"]);
          return built (cmd);
      }
      if (!a.contains ("idx"))
          return usage_err ("action '" + act + "' requires 'idx'");
      return built ("break " + act + " " + istr (a["idx"]));
  }, nullptr },

{ "palm_watch", "Watchpoint: stop when the guest WRITES the address range. On hit the CPU "
  "blocks on a Continue/Debug/Reset dialog (state=blocked_on_ui): inspect with "
  "palm_dialog/palm_backtrace, resume with palm_dialog respond=continue. One at a time.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"set", "clear", "status"})},
                            {"addr", str_prop ("Start address (set)")},
                            {"nbytes", int_prop ("Range length 1-65536 (set)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "status")
          return built ("watch status", false, true);
      if (act == "clear")
          return built ("watch clear");
      if (!a.contains ("addr") || !a.contains ("nbytes"))
          return usage_err ("action 'set' requires 'addr' and 'nbytes'");
      return built ("watch set " + sstr (a["addr"]) + " " + istr (a["nbytes"]));
  }, nullptr },

{ "palm_spy", "Step spy: stop when the VALUE at a single address changes. Raises the same "
  "Continue/Debug/Reset dialog as palm_watch on hit. One at a time.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"set", "clear", "status"})},
                            {"addr", str_prop ("Address to monitor (set)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "status")
          return built ("spy status", false, true);
      if (act == "clear")
          return built ("spy clear");
      if (!a.contains ("addr"))
          return usage_err ("action 'set' requires 'addr'");
      return built ("spy set " + sstr (a["addr"]));
  }, nullptr },

{ "palm_log", "Emulator event logging: 20 categories (action=list shows them), levels "
  "0=off 1=during-gremlins 2=always. dump flushes the buffer to file, clear empties it.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"list", "set", "dump", "clear"})},
                            {"category", str_prop ("Category name from action=list (set)")},
                            {"level", int_prop ("0=off, 1=gremlin, 2=always (set)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "list")
          return built ("log list", true, true);
      if (act == "set")
      {
          if (!a.contains ("category") || !a.contains ("level"))
              return usage_err ("action 'set' requires 'category' and 'level'");
          return built ("log set " + sstr (a["category"]) + " " + istr (a["level"]));
      }
      return built ("log " + act);
  }, nullptr },

{ "palm_gremlin", "Automated random-event stress testing (Hordes). action=new requires "
  "seed+events. WARNING: while a gremlin runs, normal input tools return "
  "'ERR busy: gremlin running' — stop the gremlin first.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"new", "status", "suspend",
                                                                "step", "resume", "stop"})},
                            {"seed", int_prop ("Random seed (new)")},
                            {"events", int_prop ("Number of events to post (new)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "status")
          return built ("gremlin status", false, true);
      if (act == "new")
      {
          if (!a.contains ("seed") || !a.contains ("events"))
              return usage_err ("action 'new' requires 'seed' and 'events'");
          return built ("gremlin new " + istr (a["seed"]) + " " + istr (a["events"]));
      }
      return built ("gremlin " + act);
  }, nullptr },

{ "palm_check", "MetaMemory access-check flags (18; action=list shows them). "
  "WARNING (landmine #7): enabling any DRAM-region flag (LowMemoryAccess, "
  "SystemGlobalAccess, ScreenAccess, MemMgrDataAccess, FreeChunkAccess, "
  "UnlockedChunkAccess) re-arms an O(n) heap scan per memory access — 100% CPU within "
  "~10 minutes. Enable briefly for a targeted test, then action=clearall.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"list", "set", "set-all", "clearall"})},
                            {"flag", str_prop ("Flag name from action=list (set)")},
                            {"on", bool_prop ("true=on, false=off (set/set-all)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "list")
          return built ("check list", true, true);
      if (act == "clearall")
          return built ("check clearall");
      if (!a.contains ("on"))
          return usage_err ("action '" + act + "' requires 'on'");
      std::string onoff = a["on"].get<bool> () ? "on" : "off";
      if (act == "set-all")
          return built ("check set-all " + onoff);
      if (!a.contains ("flag"))
          return usage_err ("action 'set' requires 'flag' and 'on'");
      return built ("check set " + sstr (a["flag"]) + " " + onoff);
  }, nullptr },

{ "palm_errorhandling", "Query or set how the emulator responds to guest errors/warnings "
  "(per-setting behavior: show dialog, auto-continue, quit, or switch).",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"get", "set"})},
                            {"setting", enum_prop ("Which setting (set)",
                                                   {"WarningOff", "ErrorOff", "WarningOn", "ErrorOn"})},
                            {"behavior", enum_prop ("Behavior (set)",
                                                    {"show", "continue", "quit", "switch"})}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      if (sstr (a["action"]) == "get")
          return built ("errorhandling get", true, true);
      if (!a.contains ("setting") || !a.contains ("behavior"))
          return usage_err ("action 'set' requires 'setting' and 'behavior'");
      return built ("errorhandling set " + sstr (a["setting"]) + " " + sstr (a["behavior"]));
  }, nullptr },

{ "palm_profile", "Metrowerks-format CPU profiler. Order matters: init -> start -> stop -> "
  "dump (requires path; writes .mwp plus a .txt sibling). cycles queries raw counters. "
  "cleanup frees profiler memory.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"init", "start", "stop", "dump",
                                                                "print", "cleanup", "cycles"})},
                            {"max", int_prop ("Max functions (init, optional; requires depth)")},
                            {"depth", int_prop ("Max stack depth (init, optional)")},
                            {"path", str_prop ("Output file path (dump/print)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "cycles")
          return built ("profile cycles", false, true);
      if (act == "init")
      {
          std::string cmd = "profile init";
          if (a.contains ("max") && a.contains ("depth"))
              cmd += " " + istr (a["max"]) + " " + istr (a["depth"]);
          else if (a.contains ("max") || a.contains ("depth"))
              return usage_err ("action 'init' takes 'max' and 'depth' together");
          return built (cmd);
      }
      if (act == "dump" || act == "print")
      {
          if (!a.contains ("path"))
              return usage_err ("action '" + act + "' requires 'path'");
          return built ("profile " + act + " " + sstr (a["path"]));
      }
      return built ("profile " + act);
  }, nullptr },

};  // kTools

static const ToolDef* find_tool (const std::string& name)
{
    for (const ToolDef& t : kTools)
        if (name == t.name)
            return &t;
    return nullptr;
}

// ============================================================================
// Dispatch — one path for every tool
// ============================================================================

static json dispatch_tool (const json& id, const std::string& name, const json& args)
{
    const ToolDef* t = find_tool (name);
    if (!t)
        return make_tool_result (id, "Unknown tool: " + name, true);

    std::string verr = validate_args (t->schema (), args);
    if (!verr.empty ())
        return make_tool_result (id, "ERR usage: " + verr, true);

    if (t->custom)
        return t->custom (id, args);

    BuiltCmd b = t->build (args);
    if (!b.err.empty ())
        return make_tool_result (id, "ERR usage: " + b.err, true);

    std::string resp = b.multiline ? rc_command_multi (b.cmd, b.idempotent)
                                   : rc_command (b.cmd, b.idempotent);
    return make_tool_result (id, resp, resp.rfind ("OK", 0) != 0);
}

// ============================================================================
// MCP method handlers
// ============================================================================

static json handle_initialize (const json& id, const json& /* params */)
{
    return make_result (id, {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", {{"tools", json::object ()}}},
        {"serverInfo", {{"name", "pose64-mcp-proxy"}, {"version", "1.0.0"}}}
    });
}

static json handle_tools_list (const json& id)
{
    json tools = json::array ();
    for (const ToolDef& t : kTools)
        tools.push_back ({{"name", t.name},
                          {"description", t.description},
                          {"inputSchema", t.schema ()}});
    return make_result (id, {{"tools", tools}});
}

static json handle_tools_call (const json& id, const json& params)
{
    std::string name = params.value ("name", "");
    json arguments = params.value ("arguments", json::object ());
    return dispatch_tool (id, name, arguments);
}

// ============================================================================
// Main
// ============================================================================

int main (int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc)
            g_port = std::atoi (argv[++i]);
        else if (arg == "--host" && i + 1 < argc)
            g_host = argv[++i];
        else if (arg == "--recv-timeout" && i + 1 < argc)
            g_recv_timeout_sec = std::atoi (argv[++i]);
        else if (arg == "--help" || arg == "-h")
        {
            fprintf (stderr,
                "Usage: pose64-mcp-proxy [--host HOST] [--port PORT] [--recv-timeout SEC]\n"
                "  --host HOST         ReControl server host (default: 127.0.0.1)\n"
                "  --port PORT         ReControl server port (default: 6416)\n"
                "  --recv-timeout SEC  per-command read timeout (default: 60)\n");
            return 0;
        }
    }

    log_msg ("ready — TCP connection deferred until first tool call");

    std::string line;
    while (std::getline (std::cin, line))
    {
        if (line.empty ())
            continue;

        // Strip trailing CR if present
        if (!line.empty () && line.back () == '\r')
            line.pop_back ();

        json msg;
        try
        {
            msg = json::parse (line);
        }
        catch (const json::exception& e)
        {
            log_msg ("invalid JSON on stdin: " + std::string (e.what ()));
            continue;
        }

        std::string method = msg.value ("method", "");
        json id = msg.contains ("id") ? msg["id"] : json (nullptr);
        json params = msg.value ("params", json::object ());

        // Notifications (no id) — handle silently
        if (id.is_null ())
        {
            // "notifications/initialized" and others — no response needed
            continue;
        }

        json response;

        if (method == "initialize")
            response = handle_initialize (id, params);
        else if (method == "tools/list")
            response = handle_tools_list (id);
        else if (method == "tools/call")
            response = handle_tools_call (id, params);
        else
            response = make_error (id, -32601, "Method not found");

        std::cout << response.dump () << "\n" << std::flush;
    }

    log_msg ("stdin closed, exiting");
    if (g_sock >= 0)
        close (g_sock);

    return 0;
}
